#include "stdafx.h"
#include "cargo_type.h"
#include "command_func.h"
#include "company_func.h"
#include "company_type.h"
#include "console_func.h"
#include "console_type.h"
#include "map_func.h"
#include "rail_type.h"
#include "tile_type.h"
#include "train.h"
#include "vehicle_base.h"
#include "vehicle_func.h"
#include "vehicle_type.h"
#include "vehiclelist.h"

#include <algorithm>
#include <utility>
#include <vector>

// Automatically upgrade rail.
//
// Step 1: Send all trains to depot.
// Step 2: Save all trains and associated routes, and sell the trains.
// Step 3: Run rail upgrade tool.
// Step 4: Buy new trains and imbue them with the old orders.
// Step 5: Start all trains.

namespace AutoUpgradeRailType {

	// all-important state
	int current_state = 0;
	// 0 means that we are not auto-upgrading
	// 1 means that we next need to send all trains to depot
	// 2 means that we already invoked the "send all" button, but need to clear up the remaining trains that can't find a path to depot
	// 3 means that we have issued all the commands to send trains to depot, and we're now just waiting for them to reach the depot and stop there
	// 4 means that all vehicles are in depot and we can save the order lists
	// 5 means that we are currently selling the trains
	// 6 means that we should now upgrade all the rail (all trains are already sold)
	// 7 means that we now need to buy the new trains

	// if awaiting response, don't do anything
	bool awaiting_response = false;

	// the rail type that we want to upgrade to
	RailType rail_type;

	// the company where we are applying auto-upgrade to (in case the user switches companies, we should stop auto-upgrade)
	CompanyID current_company;

	// tick count (for states that need it) - this is the number of ticks to the next event
	size_t ticks_remaining;
	constexpr size_t TICKS_PER_SECOND = 30;
	constexpr size_t SHORT_SECONDS = 2;

	// represents the list of carriages
	struct VehicleProperties {
		TileIndex depot; // which depot the vehicle is currently in
		std::vector<CargoID> cargos; // list of non-engine carriages and their cargo types; CT_INVALID represents an engine to replace
		size_t route_index; // index into the routes vector

		VehicleProperties(TileIndex depot, std::vector<CargoID> cargos, size_t route_index) :depot(depot), cargos(std::move(cargos)), route_index(route_index) {}
	};

	// represents a set of vehicles sharing orders
	struct Route {
		Order* order_chain; // it's not stored in an order list since that requires vehicles
		Vehicle* first_shared; // first shared new vehicle, null if there are no vehicles created yet

		Route(Order* order_chain) : order_chain(order_chain), first_shared(nullptr) {}
	};

	// storage for order list and vehicle specs
	std::vector<Route> routes;
	std::vector<VehicleProperties> vehicle_properties; // list of vehicle data to construct new vehicles later

	// storage for list of depot tiles
	std::vector<TileIndex> depots;
	std::vector<TileIndex>::const_iterator depots_it; // for step 4

	RailType ParseRailType(const char* str) {
		if (strcmp(str, "rail") == 0) {
			return RAILTYPE_RAIL;
		}
		else if (strcmp(str, "electric") == 0) {
			return RAILTYPE_ELECTRIC;
		}
		else if (strcmp(str, "monorail") == 0) {
			return RAILTYPE_MONO;
		}
		else if (strcmp(str, "maglev") == 0) {
			return RAILTYPE_MAGLEV;
		}
		else {
			return INVALID_RAILTYPE;
		}
	}

	bool Start(const char* type_str) {
		// check if we are in a valid company
		if (_local_company == COMPANY_SPECTATOR) {
			IConsolePrintF(CC_WARNING, "[Auto Upgrade] You must be in a company to do this action.");
			return false;
		}

		rail_type = ParseRailType(type_str);
		if (rail_type == INVALID_RAILTYPE) {
			IConsolePrintF(CC_ERROR, "[Auto Upgrade] Invalid rail type.");
			return false;
		}

		current_company = _local_company;
		current_state = 1;
		awaiting_response = false;
		return true;
	}

	void BailOut() {
		IConsolePrintF(CC_WARNING, "[Auto Upgrade] Bailed out at step %zu.", current_state);
		current_state = 0;
		routes.clear();
		vehicle_properties.clear();
		depots.clear();
	}

	void CompleteCallback(const CommandCost&, TileIndex, uint32, uint32, uint32) {
		awaiting_response = false;
	}

	// Step 1... send all vehicles to depot
	void DoSendAllVehiclesToDepot() {
		IConsolePrintF(CC_INFO, "[Auto Upgrade] Issuing orders for all trains to go to depot...");
		VehicleListIdentifier vli(VL_GROUP_LIST, VEH_TRAIN, _local_company);
		awaiting_response = true;
		if (!DoCommandP(0, DEPOT_MASS_SEND, vli.Pack(), GetCmdSendToDepot(VEH_TRAIN))) {
			BailOut();
			return;
		}

		ticks_remaining = SHORT_SECONDS * TICKS_PER_SECOND;
		current_state = 2;
	}

	// Step 1 (clean-up)... send remaining vehicles to depot
	void DoSendRemainingVehiclesToDepot() {
		if (--ticks_remaining > 0) return;

		size_t num_failed = 0;
		for (const Vehicle* v : Vehicle::Iterate()) {
			if (v->type == VEH_TRAIN && v->IsPrimaryVehicle() && v->owner == current_company) {
				// if it's not yet going to depot, then we should send it to the depot manually
				if (v->current_order.GetType() != OT_GOTO_DEPOT) {
					// We aren't spamming the server...
					// if it can't find a route to the local depot then the server won't even hear about it
					awaiting_response = true;
					if (DoCommandP(v->tile, v->index, 0, GetCmdSendToDepot(v))) {
						ticks_remaining = SHORT_SECONDS * TICKS_PER_SECOND;
						return;
					}
					else {
						++num_failed;
					}
				}
			}
		}

		if (num_failed > 0) {
			IConsolePrintF(CC_INFO, "[Auto Upgrade] Still have %zu trains to send to depot...", num_failed);
			ticks_remaining = SHORT_SECONDS * TICKS_PER_SECOND;
		}
		else {
			IConsolePrintF(CC_INFO, "[Auto Upgrade] Done issuing all orders to go to depot.");
			IConsolePrintF(CC_INFO, "[Auto Upgrade] Waiting for all trains to stop in depot...");
			// no need to set ticks_remaining because step 3 doesn't use it
			current_state = 3;
		}
	}

	void DoWaitUntilAllVehiclesAreStoppedInDepot() {
		for (const Vehicle* v : Vehicle::Iterate()) {
			if (v->type == VEH_TRAIN && v->IsPrimaryVehicle() && v->owner == current_company) {
				if (!v->IsStoppedInDepot()) {
					return;
				}
			}
		}

		// all vehicles are stopped in depot
		ticks_remaining = SHORT_SECONDS * TICKS_PER_SECOND;
		current_state = 4;
	}

	void DoSaveOrderLists() {
		if (--ticks_remaining > 0) return;

		assert(routes.empty());
		assert(vehicle_properties.empty());

		std::vector<const Vehicle*> vehicles;
		for (const Vehicle* v : Vehicle::Iterate()) {
			if (v->type == VEH_TRAIN && v->IsPrimaryVehicle() && v->owner == current_company) {
				vehicles.push_back(v);
			}
		}

		std::sort(vehicles.begin(), vehicles.end(), [](const Vehicle* u, const Vehicle* v) {
			return u->FirstShared() < v->FirstShared();
		});

		auto begin = vehicles.cbegin();
		while (begin != vehicles.cend()) {
			auto end = std::find_if_not(begin, vehicles.cend(), [first_shared = (*begin)->FirstShared()](const Vehicle* v) {
				return v->FirstShared() == first_shared;
			});

			// get route index
			const size_t route_index = routes.size();

			// add route
			Order* new_order_chain;
			{
				Order** new_order_last = &new_order_chain;
				for (const Order* order : (*begin)->Orders()) {
					*new_order_last = new Order();
					(*new_order_last)->AssignOrder(*order);
					new_order_last = &((*new_order_last)->next);
				}
				*new_order_last = nullptr;
			}
			routes.emplace_back(new_order_chain);

			// add vehicle properties
			for (; begin != end; ++begin) {
				std::vector<CargoID> cargos;
				for (const Train* t = Train::From(*begin); t; t = t->GetNextUnit()) {
					if (t->IsEngine()) {
						cargos.push_back(CT_INVALID);
					}
					else {
						cargos.push_back(t->cargo_type);
					}
				}
				const TileIndex depot = (*begin)->tile;
				vehicle_properties.emplace_back(depot, std::move(cargos), route_index);
				depots.push_back(depot);
			}
		}

		std::sort(depots.begin(), depots.end());
		depots.erase(std::unique(depots.begin(), depots.end()), depots.end());
		depots_it = depots.cbegin();

		ticks_remaining = SHORT_SECONDS * TICKS_PER_SECOND;
		current_state = 5;
	}

	void DoSellAllVehicles() {
		if (--ticks_remaining > 0) return;

		if (depots_it != depots.end()) {
			awaiting_response = true;
			if (!DoCommandP(*depots_it, VEH_TRAIN, 0, CMD_DEPOT_SELL_ALL_VEHICLES)) {
				BailOut();
			}
			++depots_it;
			ticks_remaining = SHORT_SECONDS * TICKS_PER_SECOND;
		}

		ticks_remaining = SHORT_SECONDS * TICKS_PER_SECOND;
		current_state = 6;
	}

	void DoUpgradeRail() {
		if (--ticks_remaining > 0) return;

		const uint min_coord = _settings_game.construction.freeform_edges ? 1 : 0;
		awaiting_response = true;
		DoCommandP(TileXY(MapMaxX() - 1, MapMaxY() - 1), TileXY(min_coord, min_coord), rail_type, CMD_CONVERT_RAIL);

		ticks_remaining = SHORT_SECONDS * TICKS_PER_SECOND;
		current_state = 7;
	}

	void DoBuyAllVehicles() {
		// todo
	}

	// later: DoCommandP(this->window_number, sel_eng | (cargo << 24), 0, GetCmdBuildVeh(this->vehicle_type), callback);

	// Called once per tick (= 1/30 seconds)
	void OnTick() {
		if (current_state != 0 && _local_company != current_company) {
			BailOut();
		}
		if (awaiting_response) return;
		switch (current_state) {
		case 0:
			break;
		case 1:
			DoSendAllVehiclesToDepot();
			break;
		case 2:
			DoSendRemainingVehiclesToDepot();
			break;
		case 3:
			DoWaitUntilAllVehiclesAreStoppedInDepot();
			break;
		case 4:
			DoSaveOrderLists();
			break;
		case 5:
			DoSellAllVehicles();
			break;
		case 6:
			DoUpgradeRail();
			break;
		case 7:
			DoBuyAllVehicles();
			break;
		}
	}

}
