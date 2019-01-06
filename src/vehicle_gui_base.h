/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file vehicle_gui_base.h Functions/classes shared between the different vehicle list GUIs. */

#ifndef VEHICLE_GUI_BASE_H
#define VEHICLE_GUI_BASE_H

#include "core/smallvec_type.hpp"
#include "date_type.h"
#include "economy_type.h"
#include "sortlist_type.h"
#include "vehiclelist.h"
#include "window_gui.h"
#include "widgets/dropdown_type.h"

#include <algorithm>
#include <iterator>

/**
 * Represents a group of any number of vehicles.
 */
struct GUIVehicleGroup {

private:
	uint num_vehicles;              ///< Number of elements in this list.  Only used when this the grouping is not GB_NONE.
	Money display_profit_this_year;
	Money display_profit_last_year;
	Date age;                       ///< Age in days.

public:
	enum {
		LIST_SIZE = 3
	};

	const Vehicle *vehicles[LIST_SIZE];

	const Vehicle * const & GetSingleVehicle() const;
	void Reset();
	void Append(const Vehicle *vehicle);
	uint NumVehiclesForDisplay() const;
	uint RealNumVehicles() const;
	void AssignSingleVehicle(const Vehicle* vehicle);
	Money GetDisplayProfitThisYear() const;
	Money GetDisplayProfitLastYear() const;
	Date GetAge() const;
};

typedef GUIList<GUIVehicleGroup> GUIVehicleGroupList;
typedef GUIList<const Vehicle*> GUIVehicleList;

struct BaseVehicleListWindow : public Window {

	enum GroupBy : byte {
		GB_NONE,
		GB_SHARED_ORDERS,

		GB_END,
	};

	GroupBy grouping;               ///< How we want to group the list.
	GUIVehicleGroupList vehgroups;  ///< List of (groups of) vehicles.
	Listing *sorting;               ///< Pointer to the vehicle type related sorting.
	byte unitnumber_digits;         ///< The number of digits of the highest unit number.
	Scrollbar *vscroll;
	VehicleListIdentifier vli;      ///< Identifier of the vehicle list we want to currently show.

	typedef GUIVehicleGroupList::SortFunction VehicleGroupSortFunction;
	typedef GUIVehicleList::SortFunction VehicleIndividualSortFunction;

	enum ActionDropdownItem {
		ADI_REPLACE,
		ADI_SERVICE,
		ADI_DEPOT,
		ADI_ADD_SHARED,
		ADI_REMOVE_ALL,
	};

	static const StringID vehicle_depot_name[];
	static const StringID vehicle_group_by_names[];
	static const StringID vehicle_group_none_sorter_names[];
	static const StringID vehicle_group_shared_orders_sorter_names[];
	static VehicleGroupSortFunction * const vehicle_group_none_sorter_funcs[];
	static VehicleGroupSortFunction * const vehicle_group_shared_orders_sorter_funcs[];

	BaseVehicleListWindow(WindowDesc *desc, WindowNumber wno) : Window(desc), vli(VehicleListIdentifier::UnPack(wno))
	{
		// TODO: retrieve the user preference from somewhere instead of intializing to GB_NONE for every new window.
		this->grouping = GB_NONE;
		this->UpdateSortingFromGrouping();
	}

	void UpdateSortingFromGrouping();

	void DrawVehicleListItems(VehicleID selected_vehicle, int line_height, const Rect &r) const;
	void UpdateVehicleGroupBy(GroupBy group_by);
	void SortVehicleList();
	void BuildVehicleList();
	Dimension GetActionDropdownSize(bool show_autoreplace, bool show_group);
	DropDownList *BuildActionDropdownList(bool show_autoreplace, bool show_group);

	const StringID *GetVehicleSorterNames() {
		switch (this->grouping) {
		case GB_NONE:
			return vehicle_group_none_sorter_names;
		case GB_SHARED_ORDERS:
			return vehicle_group_shared_orders_sorter_names;
		}
	}

	VehicleGroupSortFunction * const *GetVehicleSorterFuncs() {
		switch (this->grouping) {
		case GB_NONE:
			return vehicle_group_none_sorter_funcs;
		case GB_SHARED_ORDERS:
			return vehicle_group_shared_orders_sorter_funcs;
		}
	}
};

uint GetVehicleListHeight(VehicleType type, uint divisor = 1);

struct Sorting {
	Listing aircraft;
	Listing roadveh;
	Listing ship;
	Listing train;
};

extern Sorting _sorting[BaseVehicleListWindow::GB_END];

#endif /* VEHICLE_GUI_BASE_H */
