#include "stdafx.h"
#include "articulated_vehicles.h"
#include "auto_upgrade_coro.hpp"
#include "cargo_type.h"
#include "command_func.h"
#include "company_func.h"
#include "company_type.h"
#include "console_func.h"
#include "console_type.h"
#include "depot_map.h"
#include "engine_func.h"
#include "map_func.h"
#include "rail_map.h"
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
	// int current_state = 0;
	// 0 means that we are not auto-upgrading
	// 1 means that we next need to send all trains to depot
	// 2 means that we already invoked the "send all" button, but need to clear up the remaining trains that can't find a path to depot
	// 3 means that we have issued all the commands to send trains to depot, and we're now just waiting for them to reach the depot and stop there
	// 4 means that all vehicles are in depot and we can save the order lists
	// 5 means that we are currently selling the trains
	// 6 means that we should now upgrade all the rail (all trains are already sold)
	// 7 means that we now need to buy the new trains
	// 8 - auxiliary state for 7

	// the rail type that we want to upgrade to
	RailType rail_type;

	// the company where we are applying auto-upgrade to (in case the user switches companies, we should stop auto-upgrade)
	CompanyID current_company = COMPANY_SPECTATOR;

	constexpr size_t TICKS_PER_SECOND = 30;
	constexpr size_t SHORT_SECONDS = 1;

	// represents the list of carriages
	struct VehicleProperties {
		TileIndex depot; // which depot the vehicle is currently in
		std::vector<CargoID> cargos; // list of non-engine carriages and their cargo types; CT_INVALID represents an engine to replace
		size_t route_index; // index into the routes vector

		VehicleProperties(TileIndex depot, std::vector<CargoID> cargos, size_t route_index) :depot(depot), cargos(std::move(cargos)), route_index(route_index) {}
	};

	// represents a set of vehicles sharing orders
	struct Route {
		std::vector<uint32> packed_orders; // orders that have been packed using order.Pack()
		Vehicle* first_shared; // first shared new vehicle, null if there are no vehicles created yet

		Route(std::vector<uint32> packed_orders) : packed_orders(packed_orders), first_shared(nullptr) {}
	};


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

	Task DoCoro();

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
		ResetCoroState();
		DoCoro();
		return true;
	}

	void BailOut() {
		IConsolePrintF(CC_ERROR, "[Auto Upgrade] Bailed out.");
		current_company = COMPANY_SPECTATOR;
	}


	CargoTypes GetRefittableCargoTypes(EngineID eid) {
		return GetUnionOfArticulatedRefitMasks(eid, true) & _standard_cargo_mask;
	}

	// returns the parameters to pass into the build vehicle command
	// for the fastest vehicle of each type
	std::pair<EngineID, CargoID> GetNewTrainUnit(CargoID cargo, CargoID wagon_cargo) {
		if (cargo == CT_INVALID) {
			// is an engine to replace

			const Engine* best = nullptr;
			for (const Engine* e : Engine::IterateType(VEH_TRAIN)) {
				EngineID eid = e->index;
				const RailVehicleInfo& rvi = e->u.rail;
				if (rvi.railtype != rail_type || !IsEngineBuildable(eid, VEH_TRAIN, current_company) || e->GetPower() == 0) continue;
				if (!best) {
					best = e;
					continue;
				}
				if (best->GetDisplayMaxSpeed() != e->GetDisplayMaxSpeed()) {
					if (best->GetDisplayMaxSpeed() < e->GetDisplayMaxSpeed()) {
						best = e;
					}
					continue;
				}
				if (best->GetPower() != e->GetPower()) {
					if (best->GetPower() < e->GetPower()) {
						best = e;
					}
					continue;
				}
				// if all else are equal, we buy the most expensive one because it's probably the best one
				if (best->GetCost() < e->GetCost()) {
					best = e;
				}
			}
			if (!best) {
				IConsolePrintF(CC_ERROR, "[Auto Upgrade] Cannot find a suitable engine.");
				return { INVALID_ENGINE, CT_INVALID };
			}
			return { best->index, HasBit(GetRefittableCargoTypes(best->index), wagon_cargo) ? wagon_cargo : CT_INVALID };
		}
		else {
			// is a wagon

			const Engine* best = nullptr;
			for (const Engine* e : Engine::IterateType(VEH_TRAIN)) {
				EngineID eid = e->index;
				const RailVehicleInfo& rvi = e->u.rail;
				if (rvi.railtype != rail_type || !IsEngineBuildable(eid, VEH_TRAIN, current_company) || e->GetPower() > 0 || !HasBit(GetRefittableCargoTypes(e->index), cargo)) continue;
				if (!best) {
					best = e;
					continue;
				}
				if (best->GetDisplayMaxSpeed() != e->GetDisplayMaxSpeed()) {
					if (best->GetDisplayMaxSpeed() < e->GetDisplayMaxSpeed()) {
						best = e;
					}
					continue;
				}
				// if all else are equal, we buy the most expensive one because it's probably the best one
				if (best->GetCost() < e->GetCost()) {
					best = e;
				}
			}
			if (!best) {
				IConsolePrintF(CC_ERROR, "[Auto Upgrade] Cannot find a suitable wagon.");
				return { INVALID_ENGINE, CT_INVALID };
			}
			return { best->index, cargo };
		}
	}

	Task DoCoro() {
		// Step 1... send all vehicles to depot
		{
			IConsolePrintF(CC_INFO, "[Auto Upgrade] Issuing orders for all trains to go to depot...");
			VehicleListIdentifier vli(VL_GROUP_LIST, VEH_TRAIN, current_company);
			if ((co_await CoroDoCommandP(0, DEPOT_MASS_SEND, vli.Pack(), GetCmdSendToDepot(VEH_TRAIN))).Failed()) {
				IConsolePrintF(CC_ERROR, "[Auto Upgrade] Failed to send all trains to depot.");
				BailOut();
				co_return;
			}
		}

		co_await WaitTicks(TICKS_PER_SECOND);

		// Step 1 (clean-up)... send remaining vehicles to depot
		while (true) {
			size_t num_failed = 0;
			for (const Vehicle* v : Vehicle::Iterate()) {
				if (v->type == VEH_TRAIN && v->IsPrimaryVehicle() && v->owner == current_company) {
					// if it's not yet going to depot, then we should send it to the depot manually
					if (v->current_order.GetType() != OT_GOTO_DEPOT) {
						// We aren't spamming the server...
						// if it can't find a route to the local depot then the server won't even hear about it
						if ((co_await CoroDoCommandP(v->tile, v->index, 0, GetCmdSendToDepot(v))).Succeeded()) {
							co_await WaitTicks(TICKS_PER_SECOND);
						}
						else {
							++num_failed;
						}
					}
				}
			}
			if (num_failed == 0) break;
			co_await WaitTicks(1);
		}

		IConsolePrintF(CC_INFO, "[Auto Upgrade] Done issuing all orders to go to depot.");

		// Wait until all vehicles are stopped in depot
		IConsolePrintF(CC_INFO, "[Auto Upgrade] Waiting for all trains to stop in depot...");
		while (true) {
			bool all_stopped = true;
			for (const Vehicle* v : Vehicle::Iterate()) {
				if (v->type == VEH_TRAIN && v->IsPrimaryVehicle() && v->owner == current_company) {
					if (!v->IsStoppedInDepot()) {
						all_stopped = false;
						break;
					}
				}
			}
			if (all_stopped) break;
			co_await WaitTicks(1);
		}

		IConsolePrintF(CC_INFO, "[Auto Upgrade] All trains are now stopped in depot.");

		co_await WaitTicks(TICKS_PER_SECOND);

		// Step 2: Save all orders and info
		std::vector<Route> routes;
		std::vector<VehicleProperties> vehicle_properties; // list of vehicle data to construct new vehicles later
		std::vector<TileIndex> depots;
		{
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
				std::vector<uint32> packed_orders;
				{
					for (const Order* order : (*begin)->Orders()) {
						packed_orders.push_back(order->Pack());
					}
				}
				routes.emplace_back(std::move(packed_orders));

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

			IConsolePrintF(CC_INFO, "[Auto Upgrade] Saved order lists.");
		}

		co_await WaitTicks(TICKS_PER_SECOND);

		IConsolePrintF(CC_INFO, "[Auto Upgrade] Selling all trains...");
		for (TileIndex depot : depots) {
			if ((co_await CoroDoCommandP(depot, VEH_TRAIN, 0, CMD_DEPOT_SELL_ALL_VEHICLES)).Failed()) {
				IConsolePrintF(CC_ERROR, "[Auto Upgrade] Cannot sell all trains in depot.");
				BailOut();
				co_return;
			}
			co_await WaitTicks(TICKS_PER_SECOND);
		}

		IConsolePrintF(CC_INFO, "[Auto Upgrade] All trains have been sold.");

		co_await WaitTicks(TICKS_PER_SECOND);

		const uint min_coord = _settings_game.construction.freeform_edges ? 1 : 0;
		if ((co_await CoroDoCommandP(TileXY(MapMaxX() - 1, MapMaxY() - 1), TileXY(min_coord, min_coord), rail_type, CMD_CONVERT_RAIL)).Failed()) {
			IConsolePrintF(CC_ERROR, "[Auto Upgrade] Failed to do a whole-map track upgrade.");
			BailOut();
			co_return;
		}
		IConsolePrintF(CC_INFO, "[Auto Upgrade] Tracks have been upgraded.");

		co_await WaitTicks(TICKS_PER_SECOND);

		IConsolePrintF(CC_INFO, "[Auto Upgrade] Buying new vehicles...");

		// we buy the best engines and best wagons available for each train,
		// where "best" means the one that is fastest, and to break ties we buy the most expensive one.

		for (const VehicleProperties& prop : vehicle_properties) {
			if (GetRailType(prop.depot) != rail_type) {
				IConsolePrintF(CC_ERROR, "[Auto Upgrade] Somehow, depot was not upgraded.");
				BailOut();
				co_return;
			}
			Route& route = routes[prop.route_index];
			Vehicle* new_train;
			if (route.first_shared) {
				// has existing train on this route, we should clone it to share orders
				// DoCommandP(this->window_number, v->index, 1, CMD_CLONE_VEHICLE, nullptr);
				if ((co_await CoroDoCommandP(prop.depot, route.first_shared->index, 1, CMD_CLONE_VEHICLE)).Failed()) {
					IConsolePrintF(CC_ERROR, "[Auto Upgrade] Cannot share orders.");
					BailOut();
					co_return;
				}
				new_train = Vehicle::Get(_new_vehicle_id)->First(); // does this work for cloned vehicles?
				co_await WaitTicks(TICKS_PER_SECOND);
			}
			else {
				// no existing vehicle, we have to manually build it

				// the cargo that our engines should be refitted to
				CargoID wagon_cargo = CT_INVALID;
				for (CargoID cargo : prop.cargos) {
					if (cargo != CT_INVALID) {
						wagon_cargo = cargo;
						break;
					}
				}

				// build all units
				Vehicle* new_head = nullptr;
				for (CargoID cargo : prop.cargos) {
					const auto [engine_id, cargo_id] = GetNewTrainUnit(cargo, wagon_cargo);
					if (engine_id == INVALID_ENGINE) {
						IConsolePrintF(CC_ERROR, "[Auto Upgrade] Cannot find a suitable engine/wagon for cargo = %u.", static_cast<unsigned>(cargo));
						BailOut();
						co_return;
					}
					if ((co_await CoroDoCommandP(prop.depot, engine_id | (cargo_id << 24), 0, GetCmdBuildVeh(VEH_TRAIN))).Failed()) {
						IConsolePrintF(CC_ERROR, "[Auto Upgrade] Cannot build engine/wagon.");
						BailOut();
						co_return;
					}
					Vehicle* new_wagon = Vehicle::Get(_new_vehicle_id);
					co_await WaitTicks(TICKS_PER_SECOND);
					if (!new_head) {
						new_head = new_wagon;
					}
					else {
						// move the vehicle if not already in the chain
						if (new_head != new_wagon->First()) {
							if ((co_await CoroDoCommandP(prop.depot, new_wagon->index, new_head->Last()->index, CMD_MOVE_RAIL_VEHICLE)).Failed()) {
								IConsolePrintF(CC_ERROR, "[Auto Upgrade] Cannot move wagon to train.");
								BailOut();
								co_return;
							}
							co_await WaitTicks(TICKS_PER_SECOND);
						}
					}
				}
				if (!new_head) {
					IConsolePrintF(CC_ERROR, "[Auto Upgrade] Somehow, there are zero vehicles in the new train.");
					BailOut();
					co_return;
				}

				// add the new orders
				for (size_t i = 0; i != route.packed_orders.size(); ++i) {
					if ((co_await CoroDoCommandP(prop.depot, new_head->index + (static_cast<uint32>(i) << 20), route.packed_orders[i], CMD_INSERT_ORDER)).Failed()) {
						IConsolePrintF(CC_ERROR, "[Auto Upgrade] Cannot insert order.");
						BailOut();
						co_return;
					}
					co_await WaitTicks(TICKS_PER_SECOND);
				}

				// assign it
				new_train = route.first_shared = new_head;
			}

			// skip orders to the correct location, if this depot is in the order list
			uint32 depot_order_index = 0;
			for (const Order* order : new_train->Orders()) {
				if (order->IsType(OT_GOTO_DEPOT) && order->GetDestination() == GetDepotIndex(prop.depot)) break;
				++depot_order_index;
			}
			if (depot_order_index < new_train->GetNumOrders()) {
				// depot is in the order list
				if (new_train->cur_real_order_index != depot_order_index) {
					if ((co_await CoroDoCommandP(prop.depot, new_train->index, depot_order_index, CMD_SKIP_TO_ORDER)).Failed()) {
						IConsolePrintF(CC_ERROR, "[Auto Upgrade] Cannot skip to order.");
						BailOut();
						co_return;
					}
				}
			}
		}

		// start all vehicles
		{
			VehicleListIdentifier vli(VL_GROUP_LIST, VEH_TRAIN, current_company);
			if ((co_await CoroDoCommandP(0, (1 << 1) | (1 << 0), vli.Pack(), CMD_MASS_START_STOP)).Failed()) {
				IConsolePrintF(CC_ERROR, "[Auto Upgrade] Cannot start all trains.");
				BailOut();
				co_return;
			}
		}

		IConsolePrintF(CC_INFO, "[Auto Upgrade] Auto upgrade complete!");

		// set to COMPANY_SPECTATOR to say that the coro is no longer running
		current_company = COMPANY_SPECTATOR;
	}

	// Called once per tick (= 1/30 seconds)
	void OnTick() {
		if (current_company != COMPANY_SPECTATOR) {
			if (_local_company != current_company) {
				IConsolePrintF(CC_ERROR, "[Auto Upgrade] Company changed.");
				BailOut();
				return;
			}
			HandleCoro();
		}
	}

}
