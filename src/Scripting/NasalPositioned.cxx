// NasalPositioned.cxx -- expose FGPositioned classes to Nasal
//
// Written by James Turner, started 2012.
//
// Copyright (C) 2012 James Turner
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#include "config.h"

#include <cstring>

#include "NasalPositioned.hxx"

#include <simgear/sg_inlines.h>
#include <simgear/scene/material/mat.hxx>
#include <simgear/magvar/magvar.hxx>
#include <simgear/timing/sg_time.hxx>
#include <simgear/bucket/newbucket.hxx>

#include <Airports/runways.hxx>
#include <Airports/airport.hxx>
#include <Airports/dynamics.hxx>
#include <Airports/parking.hxx>
#include <Scripting/NasalSys.hxx>
#include <Navaids/navlist.hxx>
#include <Navaids/procedure.hxx>
#include <Main/globals.hxx>
#include <Main/fg_props.hxx>
#include <Main/util.hxx>
#include <Scenery/scenery.hxx>
#include <ATC/CommStation.hxx>
#include <Navaids/FlightPlan.hxx>
#include <Navaids/waypoint.hxx>
#include <Navaids/fix.hxx>
#include <Autopilot/route_mgr.hxx>
#include <Navaids/routePath.hxx>
#include <Navaids/procedure.hxx>
#include <Navaids/airways.hxx>
#include <Navaids/NavDataCache.hxx>

using namespace flightgear;

static void positionedGhostDestroy(void* g);
static void wayptGhostDestroy(void* g);
static void legGhostDestroy(void* g);
static void routeBaseGhostDestroy(void* g);

///static naGhostType PositionedGhostType = { positionedGhostDestroy, "positioned", nullptr, nullptr };

static const char* airportGhostGetMember(naContext c, void* g, naRef field, naRef* out);
static naGhostType AirportGhostType = { positionedGhostDestroy, "airport", airportGhostGetMember, nullptr };

static const char* navaidGhostGetMember(naContext c, void* g, naRef field, naRef* out);
static naGhostType NavaidGhostType = { positionedGhostDestroy, "navaid", navaidGhostGetMember, nullptr };

static const char* runwayGhostGetMember(naContext c, void* g, naRef field, naRef* out);
static naGhostType RunwayGhostType = { positionedGhostDestroy, "runway", runwayGhostGetMember, nullptr };
static naGhostType HelipadGhostType = { positionedGhostDestroy, "helipad", runwayGhostGetMember, nullptr };
static naGhostType TaxiwayGhostType = { positionedGhostDestroy, "taxiway", runwayGhostGetMember, nullptr };

static const char* fixGhostGetMember(naContext c, void* g, naRef field, naRef* out);
static naGhostType FixGhostType = { positionedGhostDestroy, "fix", fixGhostGetMember, nullptr };

static const char* wayptGhostGetMember(naContext c, void* g, naRef field, naRef* out);
static void waypointGhostSetMember(naContext c, void* g, naRef field, naRef value);

static naGhostType WayptGhostType = { wayptGhostDestroy,
  "waypoint",
  wayptGhostGetMember,
  waypointGhostSetMember};

static const char* legGhostGetMember(naContext c, void* g, naRef field, naRef* out);
static void legGhostSetMember(naContext c, void* g, naRef field, naRef value);

static naGhostType FPLegGhostType = { legGhostDestroy,
  "flightplan-leg",
  legGhostGetMember,
  legGhostSetMember};

static const char* flightplanGhostGetMember(naContext c, void* g, naRef field, naRef* out);
static void flightplanGhostSetMember(naContext c, void* g, naRef field, naRef value);

static naGhostType FlightPlanGhostType = { routeBaseGhostDestroy,
  "flightplan",
  flightplanGhostGetMember,
  flightplanGhostSetMember
};

static const char* procedureGhostGetMember(naContext c, void* g, naRef field, naRef* out);
static naGhostType ProcedureGhostType = { routeBaseGhostDestroy,
  "procedure",
  procedureGhostGetMember,
  0};

static const char* airwayGhostGetMember(naContext c, void* g, naRef field, naRef* out);
static naGhostType AirwayGhostType = { routeBaseGhostDestroy,
  "airway",
  airwayGhostGetMember,
  0};

static void hashset(naContext c, naRef hash, const char* key, naRef val)
{
  naRef s = naNewString(c);
  naStr_fromdata(s, (char*)key, strlen(key));
  naHash_set(hash, s, val);
}

static naRef stringToNasal(naContext c, const std::string& s)
{
    return naStr_fromdata(naNewString(c),
                   const_cast<char *>(s.c_str()),
                   s.length());
}

static bool convertToNum(naRef v, double& result)
{
    naRef n = naNumValue(v);
    if (naIsNil(n)) {
        return false; // couldn't convert
    }

    result = n.num;
    return true;
}

static WayptFlag wayptFlagFromString(const char* s)
{
  if (!strcmp(s, "sid")) return WPT_DEPARTURE;
  if (!strcmp(s, "star")) return WPT_ARRIVAL;
  if (!strcmp(s, "approach")) return WPT_APPROACH;
  if (!strcmp(s, "missed")) return WPT_MISS;
  if (!strcmp(s, "pseudo")) return WPT_PSEUDO;

  return (WayptFlag) 0;
}

static naRef wayptFlagToNasal(naContext c, unsigned int flags)
{
  if (flags & WPT_PSEUDO) return stringToNasal(c, "pseudo");
  if (flags & WPT_DEPARTURE) return stringToNasal(c, "sid");
  if (flags & WPT_ARRIVAL) return stringToNasal(c, "star");
  if (flags & WPT_MISS) return stringToNasal(c, "missed");
  if (flags & WPT_APPROACH) return stringToNasal(c, "approach");
  return naNil();
}

static FGPositioned* positionedGhost(naRef r)
{
    if ((naGhost_type(r) == &AirportGhostType) ||
        (naGhost_type(r) == &NavaidGhostType) ||
        (naGhost_type(r) == &RunwayGhostType) ||
        (naGhost_type(r) == &FixGhostType))
    {
        return (FGPositioned*) naGhost_ptr(r);
    }

    return 0;
}

static FGAirport* airportGhost(naRef r)
{
  if (naGhost_type(r) == &AirportGhostType)
    return (FGAirport*) naGhost_ptr(r);
  return 0;
}

static FGNavRecord* navaidGhost(naRef r)
{
  if (naGhost_type(r) == &NavaidGhostType)
    return (FGNavRecord*) naGhost_ptr(r);
  return 0;
}

static FGRunway* runwayGhost(naRef r)
{
  if (naGhost_type(r) == &RunwayGhostType)
    return (FGRunway*) naGhost_ptr(r);
  return 0;
}

static FGTaxiway* taxiwayGhost(naRef r)
{
  if (naGhost_type(r) == &TaxiwayGhostType)
    return (FGTaxiway*) naGhost_ptr(r);
  return 0;
}

static FGFix* fixGhost(naRef r)
{
  if (naGhost_type(r) == &FixGhostType)
    return (FGFix*) naGhost_ptr(r);
  return 0;
}


static void positionedGhostDestroy(void* g)
{
    FGPositioned* pos = (FGPositioned*)g;
    if (!FGPositioned::put(pos)) // unref
        delete pos;
}

static Waypt* wayptGhost(naRef r)
{
  if (naGhost_type(r) == &WayptGhostType)
    return (Waypt*) naGhost_ptr(r);

  if (naGhost_type(r) == &FPLegGhostType) {
    FlightPlan::Leg* leg = (FlightPlan::Leg*) naGhost_ptr(r);
    return leg->waypoint();
  }

  return 0;
}

static void wayptGhostDestroy(void* g)
{
  Waypt* wpt = (Waypt*)g;
  if (!Waypt::put(wpt)) // unref
    delete wpt;
}

static void legGhostDestroy(void* g)
{
  // nothing for now
}


static FlightPlan::Leg* fpLegGhost(naRef r)
{
  if (naGhost_type(r) == &FPLegGhostType)
    return (FlightPlan::Leg*) naGhost_ptr(r);
  return 0;
}

static Procedure* procedureGhost(naRef r)
{
  if (naGhost_type(r) == &ProcedureGhostType)
    return (Procedure*) naGhost_ptr(r);
  return 0;
}

static FlightPlan* flightplanGhost(naRef r)
{
  if (naGhost_type(r) == &FlightPlanGhostType)
    return (FlightPlan*) naGhost_ptr(r);
  return 0;
}

static Airway* airwayGhost(naRef r)
{
  if (naGhost_type(r) == &AirwayGhostType)
    return (Airway*) naGhost_ptr(r);
  return 0;
}

static void routeBaseGhostDestroy(void* g)
{
    RouteBase* r = (RouteBase*) g;
    if (!RouteBase::put(r)) // unref
        delete r;
}

static naRef airportPrototype;
static naRef flightplanPrototype;
static naRef geoCoordClass;
static naRef fpLegPrototype;
static naRef procedurePrototype;
static naRef airwayPrototype;

naRef ghostForAirport(naContext c, const FGAirport* apt)
{
  if (!apt) {
    return naNil();
  }

  FGPositioned::get(apt); // take a ref
  return naNewGhost2(c, &AirportGhostType, (void*) apt);
}

naRef ghostForNavaid(naContext c, const FGNavRecord* n)
{
  if (!n) {
    return naNil();
  }

  FGPositioned::get(n); // take a ref
  return naNewGhost2(c, &NavaidGhostType, (void*) n);
}

naRef ghostForRunway(naContext c, const FGRunway* r)
{
  if (!r) {
    return naNil();
  }

  FGPositioned::get(r); // take a ref
  return naNewGhost2(c, &RunwayGhostType, (void*) r);
}

naRef ghostForHelipad(naContext c, const FGHelipad* r)
{
  if (!r) {
    return naNil();
  }

  FGPositioned::get(r); // take a ref
  return naNewGhost2(c, &HelipadGhostType, (void*) r);
}

naRef ghostForTaxiway(naContext c, const FGTaxiway* r)
{
  if (!r) {
    return naNil();
  }

  FGPositioned::get(r); // take a ref
  return naNewGhost2(c, &TaxiwayGhostType, (void*) r);
}

naRef ghostForFix(naContext c, const FGFix* r)
{
  if (!r) {
    return naNil();
  }

  FGPositioned::get(r); // take a ref
  return naNewGhost2(c, &FixGhostType, (void*) r);
}

naRef ghostForPositioned(naContext c, FGPositionedRef pos)
{
    if (!pos) {
        return naNil();
    }
    
    switch (pos->type()) {
    case FGPositioned::VOR:
    case FGPositioned::NDB:
    case FGPositioned::TACAN:
    case FGPositioned::DME:
    case FGPositioned::ILS:
        return ghostForNavaid(c, fgpositioned_cast<FGNavRecord>(pos));
    case FGPositioned::FIX:
        return ghostForFix(c, fgpositioned_cast<FGFix>(pos));
    case FGPositioned::HELIPAD:
        return ghostForHelipad(c, fgpositioned_cast<FGHelipad>(pos));
    case FGPositioned::RUNWAY:
        return ghostForRunway(c, fgpositioned_cast<FGRunway>(pos));
    default:
        SG_LOG(SG_NASAL, SG_DEV_ALERT, "Type lacks Nasal ghost mapping:" << pos->typeString());
        return naNil();
    }
}

naRef ghostForWaypt(naContext c, const Waypt* wpt)
{
  if (!wpt) {
    return naNil();
  }

  Waypt::get(wpt); // take a ref
  return naNewGhost2(c, &WayptGhostType, (void*) wpt);
}

naRef ghostForLeg(naContext c, const FlightPlan::Leg* leg)
{
  if (!leg) {
    return naNil();
  }

  return naNewGhost2(c, &FPLegGhostType, (void*) leg);
}

naRef ghostForFlightPlan(naContext c, const FlightPlan* fp)
{
  if (!fp) {
    return naNil();
  }

  FlightPlan::get(fp); // take a ref
  return naNewGhost2(c, &FlightPlanGhostType, (void*) fp);
}

naRef ghostForProcedure(naContext c, const Procedure* proc)
{
  if (!proc) {
    return naNil();
  }

  FlightPlan::get(proc); // take a ref
  return naNewGhost2(c, &ProcedureGhostType, (void*) proc);
}

naRef ghostForAirway(naContext c, const Airway* awy)
{
  if (!awy) {
    return naNil();
  }

  Airway::get(awy); // take a ref
  return naNewGhost2(c, &AirwayGhostType, (void*) awy);
}


static const char* airportGhostGetMember(naContext c, void* g, naRef field, naRef* out)
{
  const char* fieldName = naStr_data(field);
  FGAirport* apt = (FGAirport*) g;

  if (!strcmp(fieldName, "parents")) {
    *out = naNewVector(c);
    naVec_append(*out, airportPrototype);
  } else if (!strcmp(fieldName, "id")) *out = stringToNasal(c, apt->ident());
  else if (!strcmp(fieldName, "name")) *out = stringToNasal(c, apt->name());
  else if (!strcmp(fieldName, "lat")) *out = naNum(apt->getLatitude());
  else if (!strcmp(fieldName, "lon")) *out = naNum(apt->getLongitude());
  else if (!strcmp(fieldName, "elevation")) {
    *out = naNum(apt->getElevation() * SG_FEET_TO_METER);
  } else if (!strcmp(fieldName, "has_metar")) {
    *out = naNum(apt->getMetar());
  } else if (!strcmp(fieldName, "runways")) {
    *out = naNewHash(c);
    double minLengthFt = fgGetDouble("/sim/navdb/min-runway-length-ft");
    for(unsigned int r=0; r<apt->numRunways(); ++r) {
      FGRunway* rwy(apt->getRunwayByIndex(r));
      // ignore unusably short runways
      if (rwy->lengthFt() < minLengthFt) {
        continue;
      }
      naRef rwyid = stringToNasal(c, rwy->ident());
      naRef rwydata = ghostForRunway(c, rwy);
      naHash_set(*out, rwyid, rwydata);
    }
  } else if (!strcmp(fieldName, "helipads")) {
    *out = naNewHash(c);

    for(unsigned int r=0; r<apt->numHelipads(); ++r) {
      FGHelipad* hp(apt->getHelipadByIndex(r));

      naRef rwyid = stringToNasal(c, hp->ident());
      naRef rwydata = ghostForHelipad(c, hp);
      naHash_set(*out, rwyid, rwydata);
    }

  } else if (!strcmp(fieldName, "taxiways")) {
    *out = naNewVector(c);
    for(unsigned int r=0; r<apt->numTaxiways(); ++r) {
      FGTaxiway* taxi(apt->getTaxiwayByIndex(r));
      naRef taxidata = ghostForTaxiway(c, taxi);
      naVec_append(*out, taxidata);
    }

  } else {
    return 0;
  }

  return "";
}

// Return the navaid ghost associated with a waypoint of navaid type.
static naRef waypointNavaid(naContext c, Waypt* wpt)
{
    FGPositioned* pos = wpt->source();
    if (!pos || (!FGNavRecord::isNavaidType(pos) && !fgpositioned_cast<FGFix>(pos))) {
        return naNil();
    }
    
    return ghostForPositioned(c, wpt->source());
}

// Return the airport ghost associated with a waypoint of airport or runway
// type.
static naRef waypointAirport(naContext c, Waypt* wpt)
{
  FGPositioned* pos = wpt->source();

  if (FGPositioned::isRunwayType(pos)) {
    pos = static_cast<FGRunway*>(pos)->airport();
  } else if (!FGPositioned::isAirportType(pos)) {
    return naNil();
  }

  return ghostForAirport(c, static_cast<FGAirport*>(pos));
}

// Return the runway ghost associated with a waypoint of runway type.
static naRef waypointRunway(naContext c, Waypt* wpt)
{
  FGPositioned* pos = wpt->source();

  if (!FGPositioned::isRunwayType(pos)) {
    return naNil();
  }

  return ghostForRunway(c, static_cast<FGRunway*>(pos));
}

static const char* waypointCommonGetMember(naContext c, Waypt* wpt, const char* fieldName, naRef* out)
{
  if (!strcmp(fieldName, "wp_name") || !strcmp(fieldName, "id")) *out = stringToNasal(c, wpt->ident());
  else if (!strcmp(fieldName, "wp_type")) *out = stringToNasal(c, wpt->type());
  else if (!strcmp(fieldName, "wp_role")) *out = wayptFlagToNasal(c, wpt->flags());
  else if (!strcmp(fieldName, "wp_lat") || !strcmp(fieldName, "lat")) *out = naNum(wpt->position().getLatitudeDeg());
  else if (!strcmp(fieldName, "wp_lon") || !strcmp(fieldName, "lon")) *out = naNum(wpt->position().getLongitudeDeg());
  else if (!strcmp(fieldName, "wp_parent_name")) {
      if (wpt->owner()) {
          *out = stringToNasal(c, wpt->owner()->ident());
      } else {
          *out = naNil();
      }
  } else if (!strcmp(fieldName, "wp_parent")) {
      // TODO add ghostForRouteElement to cover all this
    Procedure* proc = dynamic_cast<Procedure*>(wpt->owner());
    if (proc) {
      *out = ghostForProcedure(c, proc);
    } else {
      Airway* airway = dynamic_cast<Airway*>(wpt->owner());
      if (airway) {
        *out = ghostForAirway(c, airway);
      } else {
        *out = naNil();
      }
    }
  } else if (!strcmp(fieldName, "fly_type")) {
    if (wpt->type() == "hold") {
      *out = stringToNasal(c, "Hold");
    } else {
      *out = stringToNasal(c, wpt->flag(WPT_OVERFLIGHT) ? "flyOver" : "flyBy");
    }
  } else if (!strcmp(fieldName, "heading_course")) {
      *out = naNum(wpt->headingRadialDeg());
  } else if (!strcmp(fieldName, "navaid")) {
    *out = waypointNavaid(c, wpt);
  } else if (!strcmp(fieldName, "airport")) {
    *out = waypointAirport(c, wpt);
  } else if (!strcmp(fieldName, "runway")) {
    *out = waypointRunway(c, wpt);
  } else if (!strcmp(fieldName, "airway")) {
    if (wpt->type() == "via") {
      AirwayRef awy = static_cast<Via*>(wpt)->airway();
      assert(awy);
      *out = ghostForAirway(c, awy);
    } else {
      *out = naNil();
    }
  } else if (wpt->type() == "hold") {
    // hold-specific properties
    const auto hold = static_cast<Hold*>(wpt);
    if (!strcmp(fieldName, "hold_is_left_handed")) {
      *out = naNum(hold->isLeftHanded());
    } else if (!strcmp(fieldName, "hold_is_distance")) {
      *out = naNum(hold->isDistance());
    } else if (!strcmp(fieldName, "hold_is_time")) {
      *out = naNum(!hold->isDistance());
    } else if (!strcmp(fieldName, "hold_inbound_radial")) {
      *out = naNum(hold->inboundRadial());
    } else if (!strcmp(fieldName, "hold_heading_radial_deg")) {
      *out = naNum(hold->inboundRadial());
    } else if (!strcmp(fieldName, "hold_time_or_distance")) {
      // This is the leg length, defined either as a time in seconds, or a
      // distance in nm.
      *out = naNum(hold->timeOrDistance());
    } else {
      return nullptr; // member not found
    }
  } else {
    return nullptr; // member not found
  }

  return "";
}

static bool waypointCommonSetMember(naContext c, Waypt* wpt, const char* fieldName, naRef value)
{
  if (!strcmp(fieldName, "wp_role")) {
    if (!naIsString(value)) naRuntimeError(c, "wp_role must be a string");
    if (wpt->owner() != NULL) naRuntimeError(c, "cannot override wp_role on waypoint with parent");
    WayptFlag f = wayptFlagFromString(naStr_data(value));
    if (f == 0) {
      naRuntimeError(c, "unrecognized wp_role value %s", naStr_data(value));
    }

    wpt->setFlag(f, true);
  } else if (!strcmp(fieldName, "fly_type")) {
      if (!naIsString(value)) naRuntimeError(c, "fly_type must be a string");
      bool flyOver = (strcmp(naStr_data(value), "flyOver") == 0);
      wpt->setFlag(WPT_OVERFLIGHT, flyOver);
  } else if (wpt->type() == "hold") {
      const auto hold = static_cast<Hold*>(wpt);
      if (!strcmp(fieldName, "hold_heading_radial_deg")) {
          if (!naIsNum(value)) naRuntimeError(c, "set hold_heading_radial_deg: invalid hold radial");
          hold->setHoldRadial(value.num);
      } else if (!strcmp("hold_is_left_handed", fieldName)) {
          bool leftHanded = static_cast<int>(value.num) > 0;
          if (leftHanded) {
              hold->setLeftHanded();
          } else{
            hold->setRightHanded();
          }
      }
  } else {
      // nothing changed
      return false;
  }
    
    return true;
}

static const char* wayptGhostGetMember(naContext c, void* g, naRef field, naRef* out)
{
  const char* fieldName = naStr_data(field);
  Waypt* wpt = (flightgear::Waypt*) g;
  return waypointCommonGetMember(c, wpt, fieldName, out);
}

static RouteRestriction routeRestrictionFromArg(naRef arg)
{
  if (naIsNil(arg) || !naIsString(arg)) {
    return RESTRICT_NONE;
  }

  const std::string u = simgear::strutils::lowercase(naStr_data(arg));
  if (u == "computed") return RESTRICT_COMPUTED;
  if (u == "at") return RESTRICT_AT;
  if (u == "mach") return SPEED_RESTRICT_MACH;
  if (u == "computed-mach") return SPEED_COMPUTED_MACH;
  if (u == "delete") return RESTRICT_DELETE;
  return RESTRICT_NONE;
};

naRef routeRestrictionToNasal(naContext c, RouteRestriction rr)
{
  switch (rr) {
    case RESTRICT_NONE: return naNil();
    case RESTRICT_AT: return stringToNasal(c, "at");
    case RESTRICT_ABOVE: return stringToNasal(c, "above");
    case RESTRICT_BELOW: return stringToNasal(c, "below");
    case SPEED_RESTRICT_MACH: return stringToNasal(c, "mach");
    case RESTRICT_COMPUTED: return stringToNasal(c, "computed");
    case SPEED_COMPUTED_MACH: return stringToNasal(c, "computed-mach");
    case RESTRICT_DELETE: return stringToNasal(c, "delete");
  }

  return naNil();
}

// navaid() method of FPLeg ghosts
static naRef f_fpLeg_navaid(naContext c, naRef me, int argc, naRef* args)
{
  flightgear::Waypt* w = wayptGhost(me);
  if (!w) {
    naRuntimeError(c,
                   "flightplan-leg.navaid() called, but can't find the "
                   "underlying waypoint for the flightplan-leg object");
  }

  return waypointNavaid(c, w);
}

// airport() method of FPLeg ghosts
static naRef f_fpLeg_airport(naContext c, naRef me, int argc, naRef* args)
{
  flightgear::Waypt* w = wayptGhost(me);
  if (!w) {
    naRuntimeError(c,
                   "flightplan-leg.airport() called, but can't find the "
                   "underlying waypoint for the flightplan-leg object");
  }

  return waypointAirport(c, w);
}

// runway() method of FPLeg ghosts
static naRef f_fpLeg_runway(naContext c, naRef me, int argc, naRef* args)
{
  flightgear::Waypt* w = wayptGhost(me);
  if (!w) {
    naRuntimeError(c,
                   "flightplan-leg.runway() called, but can't find the "
                   "underlying waypoint for the flightplan-leg object");
  }

  return waypointRunway(c, w);
}

static const char* legGhostGetMember(naContext c, void* g, naRef field, naRef* out)
{
  const char* fieldName = naStr_data(field);
  FlightPlan::Leg* leg = (FlightPlan::Leg*) g;
  Waypt* wpt = leg->waypoint();

  if (!strcmp(fieldName, "parents")) {
    *out = naNewVector(c);
    naVec_append(*out, fpLegPrototype);
  } else if (!strcmp(fieldName, "index")) {
    *out = naNum(leg->index());
  } else if (!strcmp(fieldName, "alt_cstr")) {
    *out = naNum(leg->altitudeFt());
  } else if (!strcmp(fieldName, "alt_cstr_type")) {
    *out = routeRestrictionToNasal(c, leg->altitudeRestriction());
  } else if (!strcmp(fieldName, "speed_cstr")) {
    double s = isMachRestrict(leg->speedRestriction()) ? leg->speedMach() : leg->speedKts();
    *out = naNum(s);
  } else if (!strcmp(fieldName, "speed_cstr_type")) {
    *out = routeRestrictionToNasal(c, leg->speedRestriction());
  } else if (!strcmp(fieldName, "leg_distance")) {
    *out = naNum(leg->distanceNm());
  } else if (!strcmp(fieldName, "leg_bearing")) {
    *out = naNum(leg->courseDeg());
  } else if (!strcmp(fieldName, "distance_along_route")) {
    *out = naNum(leg->distanceAlongRoute());
  } else if (!strcmp(fieldName, "airport")) {
    *out = naNewFunc(c, naNewCCode(c, f_fpLeg_airport));
  } else if (!strcmp(fieldName, "navaid")) {
    *out = naNewFunc(c, naNewCCode(c, f_fpLeg_navaid));
  } else if (!strcmp(fieldName, "runway")) {
    *out = naNewFunc(c, naNewCCode(c, f_fpLeg_runway));
  } else if (!strcmp(fieldName, "hold_count")) {
    *out = naNum(leg->holdCount());
  } else { // check for fields defined on the underlying waypoint
    return waypointCommonGetMember(c, wpt, fieldName, out);
  }

  return ""; // success
}

static void waypointGhostSetMember(naContext c, void* g, naRef field, naRef value)
{
  const char* fieldName = naStr_data(field);
  Waypt* wpt = (Waypt*) g;
  waypointCommonSetMember(c, wpt, fieldName, value);
}

static void legGhostSetMember(naContext c, void* g, naRef field, naRef value)
{
  const char* fieldName = naStr_data(field);
  FlightPlan::Leg* leg = (FlightPlan::Leg*) g;
  
  bool didChange = false;
  if (!strcmp(fieldName, "hold_count")) {
    const int count = static_cast<int>(value.num);
    // this may upgrade the waypoint to a hold
    if (!leg->setHoldCount(count))
      naRuntimeError(c, "unable to set hold on leg waypoint: maybe unsuitable waypt type?");
  } else if (!strcmp(fieldName, "hold_heading_radial_deg")) {
    if (!leg->convertWaypointToHold())
      naRuntimeError(c, "couldn't convert leg waypoint into a hold");
    
    // now we can call the base method
    didChange = waypointCommonSetMember(c, leg->waypoint(), fieldName, value);
  } else {
    didChange = waypointCommonSetMember(c, leg->waypoint(), fieldName, value);
  }
  
  if (didChange) {
    leg->markWaypointDirty();
  }
}

static const char* flightplanGhostGetMember(naContext c, void* g, naRef field, naRef* out)
{
  const char* fieldName = naStr_data(field);
  FlightPlan* fp = static_cast<FlightPlan*>(g);

  if (!strcmp(fieldName, "parents")) {
    *out = naNewVector(c);
    naVec_append(*out, flightplanPrototype);
  }
  else if (!strcmp(fieldName, "id")) *out = stringToNasal(c, fp->ident());
  else if (!strcmp(fieldName, "departure")) *out = ghostForAirport(c, fp->departureAirport());
  else if (!strcmp(fieldName, "destination")) *out = ghostForAirport(c, fp->destinationAirport());
  else if (!strcmp(fieldName, "departure_runway")) *out = ghostForRunway(c, fp->departureRunway());
  else if (!strcmp(fieldName, "destination_runway")) *out = ghostForRunway(c, fp->destinationRunway());
  else if (!strcmp(fieldName, "sid")) *out = ghostForProcedure(c, fp->sid());
  else if (!strcmp(fieldName, "sid_trans")) *out = ghostForProcedure(c, fp->sidTransition());
  else if (!strcmp(fieldName, "star")) *out = ghostForProcedure(c, fp->star());
  else if (!strcmp(fieldName, "star_trans")) *out = ghostForProcedure(c, fp->starTransition());
  else if (!strcmp(fieldName, "approach")) *out = ghostForProcedure(c, fp->approach());
  else if (!strcmp(fieldName, "current")) *out = naNum(fp->currentIndex());
  else if (!strcmp(fieldName, "aircraftCategory")) *out = stringToNasal(c, fp->icaoAircraftCategory());
  else if (!strcmp(fieldName, "followLegTrackToFix")) *out = naNum(fp->followLegTrackToFixes());
  else if (!strcmp(fieldName, "active")) *out = naNum(fp->isActive());
  else if (!strcmp(fieldName, "cruiseAltitudeFt")) *out = naNum(fp->cruiseAltitudeFt());
  else if (!strcmp(fieldName, "cruiseFlightLevel")) *out = naNum(fp->cruiseFlightLevel());
  else if (!strcmp(fieldName, "cruiseSpeedKt")) *out = naNum(fp->cruiseSpeedKnots());
  else if (!strcmp(fieldName, "cruiseSpeedMach")) *out = naNum(fp->cruiseSpeedMach());
  else if (!strcmp(fieldName, "remarks")) *out = stringToNasal(c, fp->remarks());
  else if (!strcmp(fieldName, "callsign")) *out = stringToNasal(c, fp->callsign());
  else if (!strcmp(fieldName, "estimatedDurationMins")) *out = naNum(fp->estimatedDurationMinutes());

  else {
    return nullptr;
  }

  return "";
}

static void flightplanGhostSetMember(naContext c, void* g, naRef field, naRef value)
{
  const char* fieldName = naStr_data(field);
  FlightPlan* fp = static_cast<FlightPlan*>(g);

  if (!strcmp(fieldName, "id")) {
    if (!naIsString(value)) naRuntimeError(c, "flightplan.id must be a string");
    fp->setIdent(naStr_data(value));
  } else if (!strcmp(fieldName, "current")) {
    int index = static_cast<int>(value.num);
    if ((index < -1) || (index >= fp->numLegs())) {
      naRuntimeError(c, "flightplan.current must be a valid index or -1");
    }
    fp->setCurrentIndex(index);
  } else if (!strcmp(fieldName, "departure")) {
    FGAirport* apt = airportGhost(value);
    if (apt) {
      fp->setDeparture(apt);
      return;
    }

    FGRunway* rwy = runwayGhost(value);
    if (rwy){
      fp->setDeparture(rwy);
      return;
    }

    if (naIsNil(value)) {
      fp->clearDeparture();
      return;
    }

    naRuntimeError(c, "bad argument type setting departure");
  } else if (!strcmp(fieldName, "destination")) {
    FGAirport* apt = airportGhost(value);
    if (apt) {
      fp->setDestination(apt);
      return;
    }

    FGRunway* rwy = runwayGhost(value);
    if (rwy){
      fp->setDestination(rwy);
      return;
    }

    if (naIsNil(value)) {
      fp->clearDestination();
      return;
    }

    naRuntimeError(c, "bad argument type setting destination");
  } else if (!strcmp(fieldName, "departure_runway")) {
    FGRunway* rwy = runwayGhost(value);
    if (rwy){
      fp->setDeparture(rwy);
      return;
    }

    naRuntimeError(c, "bad argument type setting departure runway");
  } else if (!strcmp(fieldName, "destination_runway")) {
      if (naIsNil(value)) {
          fp->setDestination(static_cast<FGRunway*>(nullptr));
          return;
      }
      
    FGRunway* rwy = runwayGhost(value);
    if (rwy){
      fp->setDestination(rwy);
      return;
    }

    naRuntimeError(c, "bad argument type setting destination runway");
  } else if (!strcmp(fieldName, "sid")) {
    Procedure* proc = procedureGhost(value);
    if (proc && (proc->type() == PROCEDURE_SID)) {
      fp->setSID((flightgear::SID*) proc);
      return;
    }
    // allow a SID transition to be set, implicitly include the SID itself
    if (proc && (proc->type() == PROCEDURE_TRANSITION)) {
      fp->setSID((Transition*) proc);
      return;
    }

    if (naIsString(value)) {
      const std::string s(naStr_data(value));
      FGAirport* apt = fp->departureAirport();
      auto trans = apt->selectSIDByTransition(s);
      if (trans) {
          fp->setSID(trans);
      } else {
          fp->setSID(apt->findSIDWithIdent(s));
      }
      return;
    }

    if (naIsNil(value)) {
      fp->clearSID();
      return;
    }

    naRuntimeError(c, "bad argument type setting SID");
  } else if (!strcmp(fieldName, "star")) {
    Procedure* proc = procedureGhost(value);
    if (proc && (proc->type() == PROCEDURE_STAR)) {
      fp->setSTAR((STAR*) proc);
      return;
    }

    if (proc && (proc->type() == PROCEDURE_TRANSITION)) {
      fp->setSTAR((Transition*) proc);
      return;
    }

    if (naIsString(value)) {
      const std::string s(naStr_data(value));
      FGAirport* apt = fp->destinationAirport();
      auto trans = apt->selectSTARByTransition(s);
      if (trans) {
          fp->setSTAR(trans);
      } else {
          fp->setSTAR(apt->findSTARWithIdent(s));
      }
      return;
    }

    if (naIsNil(value)) {
      fp->clearSTAR();
      return;
    }

    naRuntimeError(c, "bad argument type setting STAR");
  } else if (!strcmp(fieldName, "approach")) {
    Procedure* proc = procedureGhost(value);
    if (proc && Approach::isApproach(proc->type())) {
      fp->setApproach((Approach*) proc);
      return;
    }

    if (naIsString(value)) {
      FGAirport* apt = fp->destinationAirport();
      fp->setApproach(apt->findApproachWithIdent(naStr_data(value)));
      return;
    }

    if (naIsNil(value)) {
      fp->setApproach(nullptr);
      return;
    }

    naRuntimeError(c, "bad argument type setting approach");
  } else if (!strcmp(fieldName, "aircraftCategory")) {
    if (!naIsString(value)) naRuntimeError(c, "aircraftCategory must be a string");
    fp->setIcaoAircraftCategory(naStr_data(value));
  } else if (!strcmp(fieldName, "followLegTrackToFix")) {
    fp->setFollowLegTrackToFixes(static_cast<bool>(value.num));
  } else if (!strcmp(fieldName, "cruiseAltitudeFt")) {
    fp->setCruiseAltitudeFt(static_cast<int>(value.num));
  } else if (!strcmp(fieldName, "cruiseFlightLevel")) {
    fp->setCruiseFlightLevel(static_cast<int>(value.num));
  } else if (!strcmp(fieldName, "cruiseSpeedKt")) {
    fp->setCruiseSpeedKnots(static_cast<int>(value.num));
  } else if (!strcmp(fieldName, "cruiseSpeedMach")) {
    fp->setCruiseSpeedMach(value.num);
  } else if (!strcmp(fieldName, "callsign")) {
    if (!naIsString(value)) naRuntimeError(c, "flightplan.callsign must be a string");
    fp->setCallsign(naStr_data(value));
  } else if (!strcmp(fieldName, "remarks")) {
    if (!naIsString(value)) naRuntimeError(c, "flightplan.remarks must be a string");
    fp->setRemarks(naStr_data(value));
  } else if (!strcmp(fieldName, "estimatedDurationMins")) {
    fp->setEstimatedDurationMinutes(static_cast<int>(value.num));
  }
}

static naRef procedureTpType(naContext c, ProcedureType ty)
{
  switch (ty) {
    case PROCEDURE_SID: return stringToNasal(c, "sid");
    case PROCEDURE_STAR: return stringToNasal(c, "star");
    case PROCEDURE_APPROACH_VOR:
    case PROCEDURE_APPROACH_ILS:
    case PROCEDURE_APPROACH_RNAV:
    case PROCEDURE_APPROACH_NDB:
      return stringToNasal(c, "IAP");
    default:
      return naNil();
  }
}

static naRef procedureRadioType(naContext c, ProcedureType ty)
{
  switch (ty) {
    case PROCEDURE_APPROACH_VOR: return stringToNasal(c, "VOR");
    case PROCEDURE_APPROACH_ILS: return stringToNasal(c, "ILS");
    case PROCEDURE_APPROACH_RNAV: return stringToNasal(c, "RNAV");
    case PROCEDURE_APPROACH_NDB: return stringToNasal(c, "NDB");
    default:
      return naNil();
  }
}

static const char* procedureGhostGetMember(naContext c, void* g, naRef field, naRef* out)
{
  const char* fieldName = naStr_data(field);
  Procedure* proc = (Procedure*) g;

  if (!strcmp(fieldName, "parents")) {
    *out = naNewVector(c);
    naVec_append(*out, procedurePrototype);
  } else if (!strcmp(fieldName, "id")) *out = stringToNasal(c, proc->ident());
  else if (!strcmp(fieldName, "airport")) *out = ghostForAirport(c, proc->airport());
  else if (!strcmp(fieldName, "tp_type")) *out = procedureTpType(c, proc->type());
  else if (!strcmp(fieldName, "radio")) *out = procedureRadioType(c, proc->type());
  else if (!strcmp(fieldName, "runways")) {
    *out = naNewVector(c);
    for (FGRunwayRef rwy : proc->runways()) {
      naVec_append(*out, stringToNasal(c, rwy->ident()));
    }
  } else if (!strcmp(fieldName, "transitions")) {
    if ((proc->type() != PROCEDURE_SID) && (proc->type() != PROCEDURE_STAR)) {
      *out = naNil();
      return "";
    }

    ArrivalDeparture* ad = static_cast<ArrivalDeparture*>(proc);
    *out = naNewVector(c);
    for (std::string id : ad->transitionIdents()) {
      naVec_append(*out, stringToNasal(c, id));
    }
  } else {
    return 0;
  }

  return "";
}

static const char* airwayGhostGetMember(naContext c, void* g, naRef field, naRef* out)
{
  const char* fieldName = naStr_data(field);
  Airway* awy = (Airway*) g;

  if (!strcmp(fieldName, "parents")) {
    *out = naNewVector(c);
    naVec_append(*out, airwayPrototype);
  } else if (!strcmp(fieldName, "id")) *out = stringToNasal(c, awy->ident());
  else if (!strcmp(fieldName, "level")) {
    const auto level = awy->level();
    switch (level) {
    case Airway::HighLevel:       *out = stringToNasal(c, "high"); break;
    case Airway::LowLevel:        *out = stringToNasal(c, "low"); break;
    case Airway::Both:            *out = stringToNasal(c, "both"); break;
    default:                      *out = naNil();
    }
  } else {
    return 0;
  }

  return "";
}

static const char* runwayGhostGetMember(naContext c, void* g, naRef field, naRef* out)
{
  const char* fieldName = naStr_data(field);
  FGRunwayBase* base = (FGRunwayBase*) g;

  if (!strcmp(fieldName, "id")) *out = stringToNasal(c, base->ident());
  else if (!strcmp(fieldName, "lat")) *out = naNum(base->latitude());
  else if (!strcmp(fieldName, "lon")) *out = naNum(base->longitude());
  else if (!strcmp(fieldName, "heading")) *out = naNum(base->headingDeg());
  else if (!strcmp(fieldName, "length")) *out = naNum(base->lengthM());
  else if (!strcmp(fieldName, "width")) *out = naNum(base->widthM());
  else if (!strcmp(fieldName, "surface")) *out = naNum(base->surface());
  else if (base->type() == FGRunwayBase::RUNWAY) {
    FGRunway* rwy = (FGRunway*) g;
    if (!strcmp(fieldName, "threshold")) *out = naNum(rwy->displacedThresholdM());
    else if (!strcmp(fieldName, "stopway")) *out = naNum(rwy->stopwayM());
    else if (!strcmp(fieldName, "reciprocal")) {
      *out = ghostForRunway(c, rwy->reciprocalRunway());
    } else if (!strcmp(fieldName, "ils_frequency_mhz")) {
      *out = rwy->ILS() ? naNum(rwy->ILS()->get_freq() / 100.0) : naNil();
    } else if (!strcmp(fieldName, "ils")) {
      *out = ghostForNavaid(c, rwy->ILS());
    } else {
      return 0;
    }
  } else {
    return 0;
  }

  return "";
}

static const char* navaidGhostGetMember(naContext c, void* g, naRef field, naRef* out)
{
  const char* fieldName = naStr_data(field);
  FGNavRecord* nav = (FGNavRecord*) g;

  if (!strcmp(fieldName, "id")) *out = stringToNasal(c, nav->ident());
  else if (!strcmp(fieldName, "name")) *out = stringToNasal(c, nav->name());
  else if (!strcmp(fieldName, "lat")) *out = naNum(nav->get_lat());
  else if (!strcmp(fieldName, "lon")) *out = naNum(nav->get_lon());
  else if (!strcmp(fieldName, "elevation")) {
    *out = naNum(nav->get_elev_ft() * SG_FEET_TO_METER);
  } else if (!strcmp(fieldName, "type")) {
    *out = stringToNasal(c, nav->nameForType(nav->type()));
  } else if (!strcmp(fieldName, "frequency")) {
    *out = naNum(nav->get_freq());
  } else if (!strcmp(fieldName, "range_nm")) {
    *out = naNum(nav->get_range());
  } else if (!strcmp(fieldName, "magvar")) {
    if (nav->type() == FGPositioned::VOR) {
      // For VORs, the multiuse function provides the magnetic variation
      double variation = nav->get_multiuse();
      SG_NORMALIZE_RANGE(variation, 0.0, 360.0);
      *out = naNum(variation);
    } else {
      *out = naNil();
    }
  } else if (!strcmp(fieldName, "colocated_dme")) {
      FGNavRecordRef dme = FGPositioned::loadById<FGNavRecord>(nav->colocatedDME());
      if (dme) {
          *out = ghostForNavaid(c, dme);
      } else {
          *out = naNil();
      }
  } else if (!strcmp(fieldName, "dme")) {
    *out = naNum(nav->hasDME());
  } else if (!strcmp(fieldName, "vortac")) {
    *out = naNum(nav->isVORTAC());
  } else if (!strcmp(fieldName, "course")) {
    if ((nav->type() == FGPositioned::ILS) || (nav->type() == FGPositioned::LOC)) {
      double radial = nav->get_multiuse();
      SG_NORMALIZE_RANGE(radial, 0.0, 360.0);
      *out = naNum(radial);
    } else {
      *out = naNil();
    }
  } else if (!strcmp(fieldName, "guid")) {
      *out = naNum(nav->guid());
  } else {
    return 0;
  }

  return "";
}

static const char* fixGhostGetMember(naContext c, void* g, naRef field, naRef* out)
{
  const char* fieldName = naStr_data(field);
  FGFix* fix = (FGFix*) g;

  if (!strcmp(fieldName, "id")) *out = stringToNasal(c, fix->ident());
  else if (!strcmp(fieldName, "lat")) *out = naNum(fix->get_lat());
  else if (!strcmp(fieldName, "lon")) *out = naNum(fix->get_lon());
    // for homogenity with other values returned by navinfo()
  else if (!strcmp(fieldName, "type")) *out = stringToNasal(c, "fix");
  else if (!strcmp(fieldName, "name")) *out = stringToNasal(c, fix->ident());
  else {
    return 0;
  }

  return "";
}

static bool hashIsCoord(naRef h)
{
  naRef parents = naHash_cget(h, (char*) "parents");
  if (!naIsVector(parents)) {
    return false;
  }

  return naEqual(naVec_get(parents, 0), geoCoordClass) != 0;
}

bool geodFromHash(naRef ref, SGGeod& result)
{
  if (!naIsHash(ref)) {
    return false;
  }


// check for manual latitude / longitude names
  naRef lat = naHash_cget(ref, (char*) "lat");
  naRef lon = naHash_cget(ref, (char*) "lon");
  if (naIsNum(lat) && naIsNum(lon)) {
    result = SGGeod::fromDeg(naNumValue(lon).num, naNumValue(lat).num);
    return true;
  }

  if (hashIsCoord(ref)) {
    naRef lat = naHash_cget(ref, (char*) "_lat");
    naRef lon = naHash_cget(ref, (char*) "_lon");
    naRef alt_feet = naHash_cget(ref, (char*) "_alt");
    if (naIsNum(lat) && naIsNum(lon) && naIsNil(alt_feet)) {
        result = SGGeod::fromRad(naNumValue(lon).num, naNumValue(lat).num);
        return true;
    }
    if (naIsNum(lat) && naIsNum(lon) && naIsNum(alt_feet)) {
        result = SGGeod::fromRadFt(naNumValue(lon).num, naNumValue(lat).num, naNumValue(alt_feet).num);
        return true;
    }
  }
// check for any synonyms?
  // latitude + longitude?

  return false;
}

static int geodFromArgs(naRef* args, int offset, int argc, SGGeod& result)
{
  if (offset >= argc) {
    return 0;
  }

  if (naIsGhost(args[offset])) {
    naGhostType* gt = naGhost_type(args[offset]);
    if (gt == &AirportGhostType) {
      result = airportGhost(args[offset])->geod();
      return 1;
    }

    if (gt == &NavaidGhostType) {
      result = navaidGhost(args[offset])->geod();
      return 1;
    }

    if (gt == &RunwayGhostType) {
      result = runwayGhost(args[offset])->geod();
      return 1;
    }

    if (gt == &TaxiwayGhostType) {
      result = taxiwayGhost(args[offset])->geod();
      return 1;
    }

    if (gt == &FixGhostType) {
      result = fixGhost(args[offset])->geod();
      return 1;
    }

    if (gt == &WayptGhostType) {
      result = wayptGhost(args[offset])->position();
      return 1;
    }
      
      if (gt == &FPLegGhostType) {
          result = fpLegGhost(args[offset])->waypoint()->position();
          return 1;
      }
  }

  if (geodFromHash(args[offset], result)) {
    return 1;
  }

  if (((argc - offset) >= 2) && naIsNum(args[offset]) && naIsNum(args[offset + 1])) {
    double lat = naNumValue(args[0]).num,
    lon = naNumValue(args[1]).num;
    result = SGGeod::fromDeg(lon, lat);
    return 2;
  }

  return 0;
}

bool vec3dFromHash(naRef ref, SGVec3d& result)
{
    if (!naIsHash(ref)) {
        return false;
    }

    // check for manual latitude / longitude names
    naRef x = naHash_cget(ref, (char*) "x");
    naRef y = naHash_cget(ref, (char*) "y");
    naRef z = naHash_cget(ref, (char*) "z");
    if (naIsNum(x) && naIsNum(y) && naIsNum(z)) {
        result = SGVec3d(naNumValue(x).num, naNumValue(y).num, naNumValue(z).num);
        return true;
    }
    return false;
}

// Convert a cartesian point to a geodetic lat/lon/altitude.
static naRef f_carttogeod(naContext c, naRef me, int argc, naRef* args)
{
  double lat, lon, alt, xyz[3];
  if(argc != 3) naRuntimeError(c, "carttogeod() expects 3 arguments");
  for(int i=0; i<3; i++)
    xyz[i] = naNumValue(args[i]).num;
  sgCartToGeod(xyz, &lat, &lon, &alt);
  lat *= SG_RADIANS_TO_DEGREES;
  lon *= SG_RADIANS_TO_DEGREES;
  naRef vec = naNewVector(c);
  naVec_append(vec, naNum(lat));
  naVec_append(vec, naNum(lon));
  naVec_append(vec, naNum(alt));
  return vec;
}

// Convert a geodetic lat/lon/altitude to a cartesian point.
static naRef f_geodtocart(naContext c, naRef me, int argc, naRef* args)
{
  if(argc != 3) naRuntimeError(c, "geodtocart() expects 3 arguments");
  double lat = naNumValue(args[0]).num * SG_DEGREES_TO_RADIANS;
  double lon = naNumValue(args[1]).num * SG_DEGREES_TO_RADIANS;
  double alt = naNumValue(args[2]).num;
  double xyz[3];
  sgGeodToCart(lat, lon, alt, xyz);
  naRef vec = naNewVector(c);
  naVec_append(vec, naNum(xyz[0]));
  naVec_append(vec, naNum(xyz[1]));
  naVec_append(vec, naNum(xyz[2]));
  return vec;
}

/**
* @name    f_get_cart_ground_intersection
* @brief   Returns where the given position in the specified direction will intersect with the ground
*
* Exposes the built in function to Nasal to allow a craft to ascertain
* whether or not a certain position and direction pair intersect with
* the ground.
*
* Useful for radars, terrain avoidance (GPWS), etc.
*
* @param [in] vec3d(x,y,z) position
* @param [in] vec3d(x,y,z) direction
*
* @retval geod hash (lat:rad,lon:rad,elevation:Meters) intersection
* @retval nil  no intersection found.
*
* Example Usage:
* @code
*     var end = geo.Coord.new(start);
*     end.apply_course_distance(heading, speed_horz_fps*FT2M);
*     end.set_alt(end.alt() - speed_down_fps*FT2M);
*
*     var dir_x = end.x() - start.x();
*     var dir_y = end.y() - start.y();
*     var dir_z = end.z() - start.z();
*     var xyz = { "x":start.x(),  "y" : start.y(),  "z" : start.z() };
*     var dir = { "x":dir_x,      "y" : dir_y,      "z" : dir_z };
*
*     var geod = get_cart_ground_intersection(xyz, dir);
*     if (geod != nil) {
*         end.set_latlon(geod.lat, geod.lon, geod.elevation);
          var dist = start.direct_distance_to(end)*M2FT;
*         var time = dist / speed_fps;
*         setprop("/sim/model/radar/time-until-impact", time);
*     }
* @endcode
*/
static naRef f_get_cart_ground_intersection(naContext c, naRef me, int argc, naRef* args)
{
	SGVec3d dir;
	SGVec3d pos;

	if (argc != 2)
		naRuntimeError(c, "geod_hash get_cart_ground_intersection(position: hash{x,y,z}, direction:hash{x,y,z}) expects 2 arguments");

	if (!vec3dFromHash(args[0], pos))
		naRuntimeError(c, "geod_hash get_cart_ground_intersection(position:hash{x,y,z}, direction:hash{x,y,z}) expects argument(0) to be hash of position containing x,y,z");

	if (!vec3dFromHash(args[1], dir))
		naRuntimeError(c, "geod_hash get_cart_ground_intersection(position: hash{x,y,z}, direction:hash{x,y,z}) expects argument(1) to be hash of direction containing x,y,z");

	SGVec3d nearestHit;
	if (!globals->get_scenery()->get_cart_ground_intersection(pos, dir, nearestHit))
		return naNil();

	const SGGeod geodHit = SGGeod::fromCart(nearestHit);

	// build a hash for returned intersection
	naRef intersection_h = naNewHash(c);
	hashset(c, intersection_h, "lat", naNum(geodHit.getLatitudeDeg()));
	hashset(c, intersection_h, "lon", naNum(geodHit.getLongitudeDeg()));
	hashset(c, intersection_h, "elevation", naNum(geodHit.getElevationM()));
	return intersection_h;
}

// convert from aircraft reference frame to global (ECEF) cartesian
static naRef f_aircraftToCart(naContext c, naRef me, int argc, naRef* args)
{
    if (argc != 1)
        naRuntimeError(c, "hash{x,y,z} aircraftToCart(position: hash{x,y,z}) expects one argument");

    SGVec3d offset;
    if (!vec3dFromHash(args[0], offset))
        naRuntimeError(c, "aircraftToCart expects argument(0) to be a hash containing x,y,z");

    double heading, pitch, roll;
    globals->get_aircraft_orientation(heading, pitch, roll);

    // Transform that one to the horizontal local coordinate system.
    SGQuatd hlTrans = SGQuatd::fromLonLat(globals->get_aircraft_position());

    // post-rotate the orientation of the aircraft wrt the horizontal local frame
    hlTrans *= SGQuatd::fromYawPitchRollDeg(heading, pitch, roll);

    // The offset converted to the usual body fixed coordinate system
    // rotated to the earth fiexed coordinates axis
    offset = hlTrans.backTransform(offset);

    SGVec3d v = globals->get_aircraft_position_cart() + offset;

    // build a hash for returned location
    naRef pos_h = naNewHash(c);
    hashset(c, pos_h, "x", naNum(v.x()));
    hashset(c, pos_h, "y", naNum(v.y()));
    hashset(c, pos_h, "z", naNum(v.z()));
    return pos_h;
}

// For given geodetic point return array with elevation, and a material data
// hash, or nil if there's no information available (tile not loaded). If
// information about the material isn't available, then nil is returned instead
// of the hash.
static naRef f_geodinfo(naContext c, naRef me, int argc, naRef* args)
{
#define HASHSET(s,l,n) naHash_set(matdata, naStr_fromdata(naNewString(c),s,l),n)
  if(argc < 2 || argc > 3)
    naRuntimeError(c, "geodinfo() expects 2 or 3 arguments: lat, lon [, maxalt]");
  double lat = naNumValue(args[0]).num;
  double lon = naNumValue(args[1]).num;
  double elev = argc == 3 ? naNumValue(args[2]).num : 10000;
  const simgear::BVHMaterial *material;
  SGGeod geod = SGGeod::fromDegM(lon, lat, elev);
    
  const auto scenery = globals->get_scenery();
  if (scenery == nullptr)
    return naNil();
    
  if(!scenery->get_elevation_m(geod, elev, &material))
    return naNil();

  naRef vec = naNewVector(c);
  naVec_append(vec, naNum(elev));

  naRef matdata = naNil();

  const SGMaterial *mat = dynamic_cast<const SGMaterial *>(material);
  if(mat) {
    matdata = naNewHash(c);
    naRef names = naNewVector(c);
    for (const std::string& n : mat->get_names())
      naVec_append(names, stringToNasal(c, n));

    HASHSET("names", 5, names);
    HASHSET("solid", 5, naNum(mat->get_solid()));
    HASHSET("friction_factor", 15, naNum(mat->get_friction_factor()));
    HASHSET("rolling_friction", 16, naNum(mat->get_rolling_friction()));
    HASHSET("load_resistance", 15, naNum(mat->get_load_resistance()));
    HASHSET("bumpiness", 9, naNum(mat->get_bumpiness()));
    HASHSET("light_coverage", 14, naNum(mat->get_light_coverage()));
  }
  naVec_append(vec, matdata);
  return vec;
#undef HASHSET
}


// Returns data hash for particular or nearest airport of a <type>, or nil
// on error.
//
// airportinfo(<id>);                   e.g. "KSFO"
// airportinfo(<type>);                 type := ("airport"|"seaport"|"heliport")
// airportinfo()                        same as  airportinfo("airport")
// airportinfo(<lat>, <lon> [, <type>]);
static naRef f_airportinfo(naContext c, naRef me, int argc, naRef* args)
{
  SGGeod pos = globals->get_aircraft_position();
  FGAirport* apt = NULL;

  if(argc >= 2 && naIsNum(args[0]) && naIsNum(args[1])) {
    pos = SGGeod::fromDeg(args[1].num, args[0].num);
    args += 2;
    argc -= 2;
  }

  double maxRange = 10000.0; // expose this? or pick a smaller value?

  FGAirport::TypeRunwayFilter filter; // defaults to airports only

  if(argc == 0) {
    // fall through and use AIRPORT
  } else if(argc == 1 && naIsString(args[0])) {
    if (filter.fromTypeString(naStr_data(args[0]))) {
      // done!
    } else {
      // user provided an <id>, hopefully
      apt = FGAirport::findByIdent(naStr_data(args[0]));
      if (!apt) {
        // return nil here, but don't raise a runtime error; this is a
        // legitamate way to validate an ICAO code, for example in a
        // dialog box or similar.
        return naNil();
      }
    }
  } else {
    naRuntimeError(c, "airportinfo() with invalid function arguments");
    return naNil();
  }

  if(!apt) {
    apt = FGAirport::findClosest(pos, maxRange, &filter);
    if(!apt) return naNil();
  }

  return ghostForAirport(c, apt);
}

static naRef f_findAirportsWithinRange(naContext c, naRef me, int argc, naRef* args)
{
  int argOffset = 0;
  SGGeod pos = globals->get_aircraft_position();
  argOffset += geodFromArgs(args, 0, argc, pos);

  if (!naIsNum(args[argOffset])) {
    naRuntimeError(c, "findAirportsWithinRange expected range (in nm) as arg %d", argOffset);
  }

  FGAirport::TypeRunwayFilter filter; // defaults to airports only
  double rangeNm = args[argOffset++].num;
  if (argOffset < argc) {
    filter.fromTypeString(naStr_data(args[argOffset++]));
  }

  naRef r = naNewVector(c);

  FGPositionedList apts = FGPositioned::findWithinRange(pos, rangeNm, &filter);
  FGPositioned::sortByRange(apts, pos);

  for (FGPositionedRef a : apts) {
    naVec_append(r, ghostForAirport(c, fgpositioned_cast<FGAirport>(a)));
  }

  return r;
}

static naRef f_findAirportsByICAO(naContext c, naRef me, int argc, naRef* args)
{
  if (!naIsString(args[0])) {
    naRuntimeError(c, "findAirportsByICAO expects string as arg 0");
  }

  int argOffset = 0;
  std::string prefix(naStr_data(args[argOffset++]));
  FGAirport::TypeRunwayFilter filter; // defaults to airports only
  if (argOffset < argc) {
    filter.fromTypeString(naStr_data(args[argOffset++]));
  }

  naRef r = naNewVector(c);

  FGPositionedList apts = FGPositioned::findAllWithIdent(prefix, &filter, false);
  for (FGPositionedRef a : apts) {
    naVec_append(r, ghostForAirport(c, fgpositioned_cast<FGAirport>(a)));
  }

  return r;
}

static naRef f_airport_tower(naContext c, naRef me, int argc, naRef* args)
{
    FGAirport* apt = airportGhost(me);
    if (!apt) {
      naRuntimeError(c, "airport.tower called on non-airport object");
    }

    // build a hash for the tower position
    SGGeod towerLoc = apt->getTowerLocation();
    naRef tower = naNewHash(c);
    hashset(c, tower, "lat", naNum(towerLoc.getLatitudeDeg()));
    hashset(c, tower, "lon", naNum(towerLoc.getLongitudeDeg()));
    hashset(c, tower, "elevation", naNum(towerLoc.getElevationM()));
    return tower;
}

static naRef f_airport_comms(naContext c, naRef me, int argc, naRef* args)
{
    FGAirport* apt = airportGhost(me);
    if (!apt) {
      naRuntimeError(c, "airport.comms called on non-airport object");
    }
    naRef comms = naNewVector(c);

// if we have an explicit type, return a simple vector of frequencies
    if (argc > 0 && !naIsString(args[0])) {
        naRuntimeError(c, "airport.comms argument must be a frequency type name");
    }

    if (argc > 0) {
        std::string commName = naStr_data(args[0]);
        FGPositioned::Type commType = FGPositioned::typeFromName(commName);

        for (auto comm : apt->commStationsOfType(commType)) {
            naVec_append(comms, naNum(comm->freqMHz()));
        }
    } else {
// otherwise return a vector of hashes, one for each comm station.
        for (auto comm : apt->commStations()) {
            naRef commHash = naNewHash(c);
            hashset(c, commHash, "frequency", naNum(comm->freqMHz()));
            hashset(c, commHash, "ident", stringToNasal(c, comm->ident()));
            naVec_append(comms, commHash);
        }
    }

    return comms;
}

static naRef f_airport_runway(naContext c, naRef me, int argc, naRef* args)
{
  FGAirport* apt = airportGhost(me);
  if (!apt) {
    naRuntimeError(c, "airport.runway called on non-airport object");
  }

  if ((argc < 1) || !naIsString(args[0])) {
    naRuntimeError(c, "airport.runway expects a runway ident argument");
  }

  std::string ident = simgear::strutils::uppercase(naStr_data(args[0]));

  if (apt->hasRunwayWithIdent(ident)) {
    return ghostForRunway(c, apt->getRunwayByIdent(ident));
  } else if (apt->hasHelipadWithIdent(ident)) {
    return ghostForHelipad(c, apt->getHelipadByIdent(ident));
  }
  return naNil();
}

static naRef f_airport_runwaysWithoutReciprocals(naContext c, naRef me, int argc, naRef* args)
{
  FGAirport* apt = airportGhost(me);
  if (!apt) {
    naRuntimeError(c, "airport.runwaysWithoutReciprocals called on non-airport object");
  }

  FGRunwayList rwylist(apt->getRunwaysWithoutReciprocals());
  naRef runways = naNewVector(c);
  for (unsigned int r=0; r<rwylist.size(); ++r) {
    FGRunway* rwy(rwylist[r]);
    naVec_append(runways, ghostForRunway(c, apt->getRunwayByIdent(rwy->ident())));
  }
  return runways;
}

static naRef f_airport_sids(naContext c, naRef me, int argc, naRef* args)
{
  FGAirport* apt = airportGhost(me);
  if (!apt) {
    naRuntimeError(c, "airport.sids called on non-airport object");
  }

  naRef sids = naNewVector(c);

  FGRunway* rwy = NULL;
  if (argc > 0 && naIsString(args[0])) {
    if (!apt->hasRunwayWithIdent(naStr_data(args[0]))) {
      return naNil();
    }

    rwy = apt->getRunwayByIdent(naStr_data(args[0]));
  } else if (argc > 0) {
    rwy = runwayGhost(args[0]);
  }

  if (rwy) {
    for (auto sid : rwy->getSIDs()) {
      naRef procId = stringToNasal(c, sid->ident());
      naVec_append(sids, procId);
    }
  } else {
    for (unsigned int s=0; s<apt->numSIDs(); ++s) {
      flightgear::SID* sid = apt->getSIDByIndex(s);
      naRef procId = stringToNasal(c, sid->ident());
      naVec_append(sids, procId);
    }
  }

  return sids;
}

static naRef f_airport_stars(naContext c, naRef me, int argc, naRef* args)
{
  FGAirport* apt = airportGhost(me);
  if (!apt) {
    naRuntimeError(c, "airport.stars called on non-airport object");
  }

  naRef stars = naNewVector(c);

  FGRunway* rwy = NULL;
  if (argc > 0 && naIsString(args[0])) {
    if (!apt->hasRunwayWithIdent(naStr_data(args[0]))) {
      return naNil();
    }

    rwy = apt->getRunwayByIdent(naStr_data(args[0]));
  } else if (argc > 0) {
    rwy = runwayGhost(args[0]);
  }

  if (rwy) {
    for (flightgear::STAR* s : rwy->getSTARs()) {
      naRef procId = stringToNasal(c, s->ident());
      naVec_append(stars, procId);
    }
  } else {
    for (unsigned int s=0; s<apt->numSTARs(); ++s) {
      flightgear::STAR* star = apt->getSTARByIndex(s);
      naRef procId = stringToNasal(c, star->ident());
      naVec_append(stars, procId);
    }
  }

  return stars;
}

static naRef f_airport_approaches(naContext c, naRef me, int argc, naRef* args)
{
  FGAirport* apt = airportGhost(me);
  if (!apt) {
    naRuntimeError(c, "airport.getApproachList called on non-airport object");
  }

  naRef approaches = naNewVector(c);

  ProcedureType ty = PROCEDURE_INVALID;
  if ((argc > 1) && naIsString(args[1])) {
    std::string u = simgear::strutils::uppercase(naStr_data(args[1]));
    if (u == "NDB") ty = PROCEDURE_APPROACH_NDB;
    if (u == "VOR") ty = PROCEDURE_APPROACH_VOR;
    if (u == "ILS") ty = PROCEDURE_APPROACH_ILS;
    if (u == "RNAV") ty = PROCEDURE_APPROACH_RNAV;
  }

  FGRunway* rwy = NULL;
  STAR* star = nullptr;
  if (argc > 0 && (rwy = runwayGhost(args[0]))) {
    // ok
  } else if (argc > 0 && (procedureGhost(args[0]))) {
      Procedure* proc = procedureGhost(args[0]);
      if (proc->type() != PROCEDURE_STAR)
          return naNil();
      star = static_cast<STAR*>(proc);
  } else if (argc > 0 && naIsString(args[0])) {
    if (!apt->hasRunwayWithIdent(naStr_data(args[0]))) {
      return naNil();
    }

    rwy = apt->getRunwayByIdent(naStr_data(args[0]));
  }

  if (rwy) {
    for (Approach* s : rwy->getApproaches()) {
      if ((ty != PROCEDURE_INVALID) && (s->type() != ty)) {
        continue;
      }

      naRef procId = stringToNasal(c, s->ident());
      naVec_append(approaches, procId);
    }
  } else if (star) {
      std::set<std::string> appIds;
      for (auto rwy : star->runways()) {
          for (auto app : rwy->getApproaches()) {
              appIds.insert(app->ident());
          }
      }
      
      for (auto s : appIds) {
          naVec_append(approaches, stringToNasal(c, s));
      }
  } else {
    // no runway specified, report them all
    RunwayVec runways;
    if (star)
        runways = star->runways();
      
    for (unsigned int s=0; s<apt->numApproaches(); ++s) {
      Approach* app = apt->getApproachByIndex(s);
      if ((ty != PROCEDURE_INVALID) && (app->type() != ty)) {
        continue;
      }
        
      naRef procId = stringToNasal(c, app->ident());
      naVec_append(approaches, procId);
    }
  }

  return approaches;
}

static naRef f_airport_parking(naContext c, naRef me, int argc, naRef* args)
{
  FGAirport* apt = airportGhost(me);
  if (!apt) {
    naRuntimeError(c, "airport.parking called on non-airport object");
  }

  naRef r = naNewVector(c);
  std::string type;
  bool onlyAvailable = false;

  if (argc > 0 && naIsString(args[0])) {
    type = naStr_data(args[0]);
  }

  if ((argc > 1) && naIsNum(args[1])) {
    onlyAvailable = (args[1].num != 0.0);
  }

  FGAirportDynamicsRef dynamics = apt->getDynamics();
  FGParkingList parkings = dynamics->getParkings(onlyAvailable, type);
  FGParkingList::const_iterator it;
  for (it = parkings.begin(); it != parkings.end(); ++it) {
    FGParkingRef park = *it;
    const SGGeod& parkLoc = park->geod();
    naRef ph = naNewHash(c);
    hashset(c, ph, "name", stringToNasal(c, park->getName()));
    hashset(c, ph, "lat", naNum(parkLoc.getLatitudeDeg()));
    hashset(c, ph, "lon", naNum(parkLoc.getLongitudeDeg()));
    hashset(c, ph, "elevation", naNum(parkLoc.getElevationM()));
    naVec_append(r, ph);
  }

  return r;
}

static naRef f_airport_getSid(naContext c, naRef me, int argc, naRef* args)
{
  FGAirport* apt = airportGhost(me);
  if (!apt) {
    naRuntimeError(c, "airport.getSid called on non-airport object");
  }

  if ((argc != 1) || !naIsString(args[0])) {
    naRuntimeError(c, "airport.getSid passed invalid argument");
  }

  std::string ident = naStr_data(args[0]);
  return ghostForProcedure(c, apt->findSIDWithIdent(ident));
}

static naRef f_airport_getStar(naContext c, naRef me, int argc, naRef* args)
{
  FGAirport* apt = airportGhost(me);
  if (!apt) {
    naRuntimeError(c, "airport.getStar called on non-airport object");
  }

  if ((argc != 1) || !naIsString(args[0])) {
    naRuntimeError(c, "airport.getStar passed invalid argument");
  }

  std::string ident = naStr_data(args[0]);
  return ghostForProcedure(c, apt->findSTARWithIdent(ident));
}

static naRef f_airport_getApproach(naContext c, naRef me, int argc, naRef* args)
{
  FGAirport* apt = airportGhost(me);
  if (!apt) {
    naRuntimeError(c, "airport.getIAP called on non-airport object");
  }

  if ((argc != 1) || !naIsString(args[0])) {
    naRuntimeError(c, "airport.getIAP passed invalid argument");
  }

  std::string ident = naStr_data(args[0]);
  return ghostForProcedure(c, apt->findApproachWithIdent(ident));
}

static naRef f_airport_findBestRunway(naContext c, naRef me, int argc, naRef* args)
{
    FGAirport* apt = airportGhost(me);
    if (!apt) {
        naRuntimeError(c, "findBestRunway called on non-airport object");
    }

    SGGeod pos;
    if (!geodFromArgs(args, 0, argc, pos)) {
        naRuntimeError(c, "findBestRunway must be passed a position");
    }

    return ghostForRunway(c,  apt->findBestRunwayForPos(pos));
}

static naRef f_airport_toString(naContext c, naRef me, int argc, naRef* args)
{
  FGAirport* apt = airportGhost(me);
  if (!apt) {
    naRuntimeError(c, "airport.tostring called on non-airport object");
  }

  return stringToNasal(c, "an airport " + apt->ident());
}

// Returns vector of data hash for navaid of a <type>, nil on error
// navaids sorted by ascending distance
// navinfo([<lat>,<lon>],[<type>],[<id>])
// lat/lon (numeric): use latitude/longitude instead of ac position
// type:              ("fix"|"vor"|"ndb"|"ils"|"dme"|"tacan"|"any")
// id:                (partial) id of the fix
// examples:
// navinfo("vor")     returns all vors
// navinfo("HAM")     return all navaids who's name start with "HAM"
// navinfo("vor", "HAM") return all vor who's name start with "HAM"
//navinfo(34,48,"vor","HAM") return all vor who's name start with "HAM"
//                           sorted by distance relative to lat=34, lon=48
static naRef f_navinfo(naContext c, naRef me, int argc, naRef* args)
{
  SGGeod pos;

  if(argc >= 2 && naIsNum(args[0]) && naIsNum(args[1])) {
    pos = SGGeod::fromDeg(args[1].num, args[0].num);
    args += 2;
    argc -= 2;
  } else {
    pos = globals->get_aircraft_position();
  }

  FGPositioned::Type type = FGPositioned::INVALID;
  nav_list_type navlist;
  const char * id = "";

  if(argc > 0 && naIsString(args[0])) {
    const char *s = naStr_data(args[0]);
    if(!strcmp(s, "any")) type = FGPositioned::INVALID;
    else if(!strcmp(s, "fix")) type = FGPositioned::FIX;
    else if(!strcmp(s, "vor")) type = FGPositioned::VOR;
    else if(!strcmp(s, "ndb")) type = FGPositioned::NDB;
    else if(!strcmp(s, "ils")) type = FGPositioned::ILS;
    else if(!strcmp(s, "dme")) type = FGPositioned::DME;
    else if(!strcmp(s, "tacan")) type = FGPositioned::TACAN;
    else id = s; // this is an id
    ++args;
    --argc;
  }

  if(argc > 0 && naIsString(args[0])) {
    if( *id != 0 ) {
      naRuntimeError(c, "navinfo() called with navaid id");
      return naNil();
    }
    id = naStr_data(args[0]);
    ++args;
    --argc;
  }

  if( argc > 0 ) {
    naRuntimeError(c, "navinfo() called with too many arguments");
    return naNil();
  }

  FGNavList::TypeFilter filter(type);
  navlist = FGNavList::findByIdentAndFreq( pos, id, 0.0, &filter );

  naRef reply = naNewVector(c);
  for( nav_list_type::const_iterator it = navlist.begin(); it != navlist.end(); ++it ) {
    naVec_append( reply, ghostForNavaid(c, *it) );
  }
  return reply;
}

static naRef f_findNavaidsWithinRange(naContext c, naRef me, int argc, naRef* args)
{
  int argOffset = 0;
  SGGeod pos = globals->get_aircraft_position();
  argOffset += geodFromArgs(args, 0, argc, pos);

  if (!naIsNum(args[argOffset])) {
    naRuntimeError(c, "findNavaidsWithinRange expected range (in nm) as arg %d", argOffset);
  }

  FGPositioned::Type type = FGPositioned::INVALID;
  double rangeNm = args[argOffset++].num;
  if (argOffset < argc) {
    type = FGPositioned::typeFromName(naStr_data(args[argOffset]));
  }

  naRef r = naNewVector(c);
  FGNavList::TypeFilter filter(type);
  FGPositionedList navs = FGPositioned::findWithinRange(pos, rangeNm, &filter);
  FGPositioned::sortByRange(navs, pos);

  for (FGPositionedRef a : navs) {
    FGNavRecord* nav = (FGNavRecord*) a.get();
    naVec_append(r, ghostForNavaid(c, nav));
  }

  return r;
}

static naRef f_findNDBByFrequency(naContext c, naRef me, int argc, naRef* args)
{
  int argOffset = 0;
  SGGeod pos = globals->get_aircraft_position();
  argOffset += geodFromArgs(args, 0, argc, pos);

  if (!naIsNum(args[argOffset])) {
    naRuntimeError(c, "findNDBByFrquency expectes frequency (in kHz) as arg %d", argOffset);
  }

  double dbFreq = args[argOffset++].num;
  FGNavList::TypeFilter filter(FGPositioned::NDB);
  nav_list_type navs = FGNavList::findAllByFreq(dbFreq, pos, &filter);
  if (navs.empty()) {
    return naNil();
  }

  return ghostForNavaid(c, navs.front().ptr());
}

static naRef f_findNDBsByFrequency(naContext c, naRef me, int argc, naRef* args)
{
  int argOffset = 0;
  SGGeod pos = globals->get_aircraft_position();
  argOffset += geodFromArgs(args, 0, argc, pos);

  if (!naIsNum(args[argOffset])) {
    naRuntimeError(c, "findNDBsByFrquency expectes frequency (in kHz) as arg %d", argOffset);
  }

  double dbFreq = args[argOffset++].num;
  FGNavList::TypeFilter filter(FGPositioned::NDB);
  nav_list_type navs = FGNavList::findAllByFreq(dbFreq, pos, &filter);
  if (navs.empty()) {
    return naNil();
  }

  naRef r = naNewVector(c);
  for (nav_rec_ptr a : navs) {
    naVec_append(r, ghostForNavaid(c, a.ptr()));
  }
  return r;
}

static naRef f_findNavaidByFrequency(naContext c, naRef me, int argc, naRef* args)
{
  int argOffset = 0;
  SGGeod pos = globals->get_aircraft_position();
  argOffset += geodFromArgs(args, 0, argc, pos);

  if (!naIsNum(args[argOffset])) {
    naRuntimeError(c, "findNavaidByFrequency expectes frequency (in Mhz) as arg %d", argOffset);
  }

  FGPositioned::Type type = FGPositioned::INVALID;
  double freqMhz = args[argOffset++].num;
  if (argOffset < argc) {
    type = FGPositioned::typeFromName(naStr_data(args[argOffset]));
    if (type == FGPositioned::NDB) {
      naRuntimeError(c, "Use findNDBByFrquency to seach NDBs");
    }
  }

  FGNavList::TypeFilter filter(type);
  auto navs = FGNavList::findAllByFreq(freqMhz, pos, &filter);
  if (navs.empty()) {
    return naNil();
  }

  return ghostForNavaid(c, navs.front().ptr());
}

static naRef f_findNavaidsByFrequency(naContext c, naRef me, int argc, naRef* args)
{
  int argOffset = 0;
  SGGeod pos = globals->get_aircraft_position();
  argOffset += geodFromArgs(args, 0, argc, pos);

  if (!naIsNum(args[argOffset])) {
    naRuntimeError(c, "findNavaidsByFrequency expectes frequency (in Mhz) as arg %d", argOffset);
  }

  FGPositioned::Type type = FGPositioned::INVALID;
  double freqMhz = args[argOffset++].num;
  if (argOffset < argc) {
    type = FGPositioned::typeFromName(naStr_data(args[argOffset]));
    if (type == FGPositioned::NDB) {
      naRuntimeError(c, "Use findNDBsByFrquency to seach NDBs");
    }
  }

  naRef r = naNewVector(c);
  FGNavList::TypeFilter filter(type);
  auto navs = FGNavList::findAllByFreq(freqMhz, pos, &filter);
  for (nav_rec_ptr a : navs) {
    naVec_append(r, ghostForNavaid(c, a.ptr()));
  }

  return r;
}

static naRef f_findNavaidsByIdent(naContext c, naRef me, int argc, naRef* args)
{
  int argOffset = 0;
  SGGeod pos = globals->get_aircraft_position();
  argOffset += geodFromArgs(args, 0, argc, pos);

  if (!naIsString(args[argOffset])) {
    naRuntimeError(c, "findNavaidsByIdent expectes ident string as arg %d", argOffset);
  }

  FGPositioned::Type type = FGPositioned::INVALID;
  std::string ident = naStr_data(args[argOffset++]);
  if (argOffset < argc) {
    type = FGPositioned::typeFromName(naStr_data(args[argOffset]));
  }

  FGNavList::TypeFilter filter(type);
  naRef r = naNewVector(c);
  nav_list_type navs = FGNavList::findByIdentAndFreq(pos, ident, 0.0, &filter);

  for (nav_rec_ptr a : navs) {
    naVec_append(r, ghostForNavaid(c, a.ptr()));
  }

  return r;
}

static naRef f_findFixesByIdent(naContext c, naRef me, int argc, naRef* args)
{
  int argOffset = 0;
  SGGeod pos = globals->get_aircraft_position();
  argOffset += geodFromArgs(args, 0, argc, pos);

  if (!naIsString(args[argOffset])) {
    naRuntimeError(c, "findFixesByIdent expectes ident string as arg %d", argOffset);
  }

  std::string ident(naStr_data(args[argOffset]));
  naRef r = naNewVector(c);

  FGPositioned::TypeFilter filter(FGPositioned::FIX);
  FGPositionedList fixes = FGPositioned::findAllWithIdent(ident, &filter);
  FGPositioned::sortByRange(fixes, pos);

  for (FGPositionedRef f : fixes) {
    naVec_append(r, ghostForFix(c, (FGFix*) f.ptr()));
  }

  return r;
}

static naRef f_findByIdent(naContext c, naRef me, int argc, naRef* args)
{
    if ((argc < 2) || !naIsString(args[0]) || !naIsString(args[1]) ) {
        naRuntimeError(c, "finxByIdent: expects ident and type as first two args");
    }
    
    std::string ident(naStr_data(args[0]));
    std::string typeSpec(naStr_data(args[1]));
    
    // optional specify search pos as final argument
    SGGeod pos = globals->get_aircraft_position();
    geodFromArgs(args, 2, argc, pos);
    FGPositioned::TypeFilter filter(FGPositioned::TypeFilter::fromString(typeSpec));
    
    naRef r = naNewVector(c);

    FGPositionedList matches = FGPositioned::findAllWithIdent(ident, &filter);
    FGPositioned::sortByRange(matches, pos);
    
    for (auto f : matches) {
        naVec_append(r, ghostForPositioned(c, f));
    }
    
    return r;
}


// Convert a cartesian point to a geodetic lat/lon/altitude.
static naRef f_magvar(naContext c, naRef me, int argc, naRef* args)
{
  SGGeod pos = globals->get_aircraft_position();
  if (argc == 0) {
    // fine, use aircraft position
  } else if (geodFromArgs(args, 0, argc, pos)) {
    // okay
  } else {
    naRuntimeError(c, "magvar() expects no arguments, a positioned hash or lat,lon pair");
  }

  double jd = globals->get_time_params()->getJD();
  double magvarDeg = sgGetMagVar(pos, jd) * SG_RADIANS_TO_DEGREES;
  return naNum(magvarDeg);
}

static naRef f_courseAndDistance(naContext c, naRef me, int argc, naRef* args)
{
    SGGeod from = globals->get_aircraft_position(), to, p;
    int argOffset = geodFromArgs(args, 0, argc, p);
    if (geodFromArgs(args, argOffset, argc, to)) {
      from = p; // we parsed both FROM and TO args, so first was from
    } else {
      to = p; // only parsed one arg, so FROM is current
    }

    if (argOffset == 0) {
        naRuntimeError(c, "invalid arguments to courseAndDistance");
    }

    double course, course2, d;
    SGGeodesy::inverse(from, to, course, course2, d);

    naRef result = naNewVector(c);
    naVec_append(result, naNum(course));
    naVec_append(result, naNum(d * SG_METER_TO_NM));
    return result;
}

static naRef f_formatLatLon(naContext c, naRef me, int argc, naRef* args)
{
  SGGeod p;
  int argOffset = geodFromArgs(args, 0, argc, p);
  if (argOffset == 0) {
    naRuntimeError(c, "invalid arguments to formatLatLon, expect a geod or lat,lon");
  }
  
  simgear::strutils::LatLonFormat format =
    static_cast<simgear::strutils::LatLonFormat>(fgGetInt("/sim/lon-lat-format"));
  if (argOffset < argc && naIsNum(args[argOffset])) {
    format = static_cast<simgear::strutils::LatLonFormat>((int) args[argOffset].num);
    if (format > simgear::strutils::LatLonFormat::DECIMAL_DEGREES_SYMBOL) {
      naRuntimeError(c, "invalid lat-lon format requested");
    }
  }
  
  const auto s = simgear::strutils::formatGeodAsString(p, format);
  return stringToNasal(c, s);
}

static naRef f_parseStringAsLatLonValue(naContext c, naRef me, int argc, naRef* args)
{
  if ((argc < 1) || !naIsString(args[0])) {
    naRuntimeError(c, "Missing / bad argument to parseStringAsLatLonValue");
  }
  
  double value;
  bool ok = simgear::strutils::parseStringAsLatLonValue(naStr_data(args[0]), value);
  if (!ok) {
    return naNil();
  }
  
  return naNum(value);
}


static naRef f_greatCircleMove(naContext c, naRef me, int argc, naRef* args)
{
  SGGeod from = globals->get_aircraft_position(), to;
  int argOffset = 0;

  // complication - don't inerpret two doubles (as the only args)
  // as a lat,lon pair - only do so if we have at least three args.
  if (argc > 2) {
    argOffset = geodFromArgs(args, 0, argc, from);
  }

  if ((argOffset + 1) >= argc) {
    naRuntimeError(c, "isufficent arguments to greatCircleMove");
  }

  if (!naIsNum(args[argOffset]) || !naIsNum(args[argOffset+1])) {
    naRuntimeError(c, "invalid arguments %d and %d to greatCircleMove",
                   argOffset, argOffset + 1);
  }

  double course = args[argOffset].num, course2;
  double distanceNm = args[argOffset + 1].num;
  SGGeodesy::direct(from, course, distanceNm * SG_NM_TO_METER, to, course2);

  // return geo.Coord
  naRef coord = naNewHash(c);
  hashset(c, coord, "lat", naNum(to.getLatitudeDeg()));
  hashset(c, coord, "lon", naNum(to.getLongitudeDeg()));
  return coord;
}

static naRef f_tilePath(naContext c, naRef me, int argc, naRef* args)
{
    SGGeod pos = globals->get_aircraft_position();
    geodFromArgs(args, 0, argc, pos);
    SGBucket b(pos);
    return stringToNasal(c, b.gen_base_path());
}

static naRef f_tileIndex(naContext c, naRef me, int argc, naRef* args)
{
  SGGeod pos = globals->get_aircraft_position();
  geodFromArgs(args, 0, argc, pos);
  SGBucket b(pos);
  return naNum(b.gen_index());
}

static naRef f_createFlightplan(naContext c, naRef me, int argc, naRef* args)
{
    flightgear::FlightPlanRef fp(new flightgear::FlightPlan);

    if ((argc > 0) && naIsString(args[0])) {
        SGPath path(naStr_data(args[0]));
        if (!path.exists()) {
            std::string pdata = path.utf8Str();
            naRuntimeError(c, "createFlightplan, no file at path %s", pdata.c_str());
        }

        if (!fp->load(path)) {
            SG_LOG(SG_NASAL, SG_WARN, "failed to load flight-plan from " << path);
            return naNil();
        }

    }

    return ghostForFlightPlan(c, fp.get());
}

static naRef f_flightplan(naContext c, naRef me, int argc, naRef* args)
{
    if (argc == 0) {
        FGRouteMgr* rm = static_cast<FGRouteMgr*>(globals->get_subsystem("route-manager"));
        return ghostForFlightPlan(c, rm->flightPlan());
    }

    if ((argc > 0) && naIsString(args[0])) {
        return f_createFlightplan(c, me, argc, args);
    }

    naRuntimeError(c, "bad arguments to flightplan()");
    return naNil();
}

class NasalFPDelegate : public FlightPlan::Delegate
{
public:
  NasalFPDelegate(FlightPlan* fp, FGNasalSys* sys, naRef ins) :
    _nasal(sys),
    _plan(fp),
    _instance(ins)
  {
      assert(fp);
      assert(sys);
      _gcSaveKey = _nasal->gcSave(ins);
  }

  ~NasalFPDelegate() override
  {
    _nasal->gcRelease(_gcSaveKey);
  }

  void departureChanged() override
  {
    callDelegateMethod("departureChanged");
  }

  void arrivalChanged() override
  {
    callDelegateMethod("arrivalChanged");
  }

  void waypointsChanged() override
  {
    callDelegateMethod("waypointsChanged");
  }

  void currentWaypointChanged() override
  {
    callDelegateMethod("currentWaypointChanged");
  }

  void cleared() override
  {
    callDelegateMethod("cleared");
  }

  void endOfFlightPlan() override
  {
    callDelegateMethod("endOfFlightPlan");
  }

  void activated() override
  {
    callDelegateMethod("activated");
  }
    
  void sequence() override
  {
    callDelegateMethod("sequence");
  }
private:

  void callDelegateMethod(const char* method)
  {
    naRef f;
    naContext ctx = naNewContext();

    if (naMember_cget(ctx, _instance, method, &f) != 0) {
        naRef arg[1];
        arg[0] = ghostForFlightPlan(ctx, _plan);
        _nasal->callMethod(f, _instance, 1, arg, naNil());
    }

    naFreeContext(ctx);
  }

  FGNasalSys* _nasal;
  FlightPlan* _plan;
  naRef _instance;
  int _gcSaveKey;
};


class NasalFPDelegateFactory : public FlightPlan::DelegateFactory
{
public:
  NasalFPDelegateFactory(naRef code)
  {
    _nasal = globals->get_subsystem<FGNasalSys>();
    _func = code;
    _gcSaveKey = _nasal->gcSave(_func);
  }

  virtual ~NasalFPDelegateFactory()
  {
    _nasal->gcRelease(_gcSaveKey);
  }

  FlightPlan::Delegate* createFlightPlanDelegate(FlightPlan* fp) override
  {
    naRef args[1];
    naContext ctx = naNewContext();
    args[0] = ghostForFlightPlan(ctx, fp);
    naRef instance = _nasal->call(_func, 1, args, naNil());

      FlightPlan::Delegate* result = nullptr;
      if (!naIsNil(instance)) {
          // will GC-save instance
          result = new NasalFPDelegate(fp, _nasal, instance);
      }

      naFreeContext(ctx);
      return result;
  }
private:
  FGNasalSys* _nasal;
  naRef _func;
  int _gcSaveKey;
};

static std::vector<NasalFPDelegateFactory*> static_nasalDelegateFactories;

void shutdownNasalPositioned()
{
    std::vector<NasalFPDelegateFactory*>::iterator it;
    for (it = static_nasalDelegateFactories.begin();
         it != static_nasalDelegateFactories.end(); ++it)
    {
        FlightPlan::unregisterDelegateFactory(*it);
        delete (*it);
    }
    static_nasalDelegateFactories.clear();
}

static naRef f_registerFPDelegate(naContext c, naRef me, int argc, naRef* args)
{
  if ((argc < 1) || !naIsFunc(args[0])) {
    naRuntimeError(c, "non-function argument to registerFlightPlanDelegate");
  }
  NasalFPDelegateFactory* factory = new NasalFPDelegateFactory(args[0]);
  FlightPlan::registerDelegateFactory(factory);
    static_nasalDelegateFactories.push_back(factory);
  return naNil();
}

static WayptRef wayptFromArg(naRef arg)
{
  WayptRef r = wayptGhost(arg);
  if (r.valid()) {
    return r;
  }

  FGPositioned* pos = positionedGhost(arg);
  if (!pos) {
    // let's check if the arg is hash, coudl extra a geod and hence build
    // a simple waypoint

    return WayptRef();
  }

// special-case for runways
  if (pos->type() == FGPositioned::RUNWAY) {
    return new RunwayWaypt((FGRunway*) pos, NULL);
  }

  return new NavaidWaypoint(pos, NULL);
}

static naRef convertWayptVecToNasal(naContext c, const WayptVec& wps)
{
  naRef result = naNewVector(c);
  for (WayptRef wpt : wps) {
    naVec_append(result, ghostForWaypt(c, wpt.get()));
  }
  return result;
}

static naRef f_airwaySearch(naContext c, naRef me, int argc, naRef* args)
{
  if (argc < 2) {
    naRuntimeError(c, "airwaysSearch needs at least two arguments");
  }

  WayptRef start = wayptFromArg(args[0]),
    end = wayptFromArg(args[1]);

  if (!start || !end) {
    SG_LOG(SG_NASAL, SG_WARN, "airwaysSearch: start or end points are invalid");
    return naNil();
  }

  bool highLevel = true;
  if ((argc > 2) && naIsString(args[2])) {
    if (!strcmp(naStr_data(args[2]), "lowlevel")) {
      highLevel = false;
    }
  }

  WayptVec route;
  if (highLevel) {
    Airway::highLevel()->route(start, end, route);
  } else {
    Airway::lowLevel()->route(start, end, route);
  }

  return convertWayptVecToNasal(c, route);
}

static FGPositionedRef positionedFromArg(naRef ref)
{
  if (!naIsGhost(ref)) 
    return {};

  naGhostType* gt = naGhost_type(ref);
  if (gt == &AirportGhostType)
    return airportGhost(ref);

  if (gt == &NavaidGhostType)
    return navaidGhost(ref);

  if (gt == &RunwayGhostType)
    return runwayGhost(ref);

  if (gt == &TaxiwayGhostType)
    return taxiwayGhost(ref);

  if (gt == &FixGhostType)
    return fixGhost(ref);

  if ((gt == &WayptGhostType) || (gt == &FPLegGhostType))
    return wayptGhost(ref)->source();

  return {};
}

static naRef f_findAirway(naContext c, naRef me, int argc, naRef* args)
{
  if ((argc < 1) || !naIsString(args[0])) {
    naRuntimeError(c, "findAirway needs at least one string arguments");
  }

  std::string ident = naStr_data(args[0]);
  FGPositionedRef pos;
  Airway::Level level = Airway::Both;
  if (argc >= 2) {
    pos = positionedFromArg(args[1]);
    if (naIsString(args[1])) {
      // level spec, 
    }
  }

  AirwayRef awy;
  if (pos) {
    SG_LOG(SG_NASAL, SG_INFO, "Pevious navaid for airway():" << pos->ident());
    awy = Airway::findByIdentAndNavaid(ident, pos);
  } else {
    awy = Airway::findByIdent(ident, level);
  }

  if (!awy)
    return naNil();

  return ghostForAirway(c, awy.get());
}


static naRef f_createWP(naContext c, naRef me, int argc, naRef* args)
{
  SGGeod pos;
  int argOffset = geodFromArgs(args, 0, argc, pos);

  if (((argc - argOffset) < 1) || !naIsString(args[argOffset])) {
    naRuntimeError(c, "createWP: no identifier supplied");
  }

  std::string ident = naStr_data(args[argOffset++]);
  WayptRef wpt = new BasicWaypt(pos, ident, NULL);

// set waypt flags - approach, departure, pseudo, etc
  if (argc > argOffset) {
    WayptFlag f = wayptFlagFromString(naStr_data(args[argOffset++]));
    if (f == 0) {
      naRuntimeError(c, "createWP: bad waypoint role");
    }

    wpt->setFlag(f);
  }

  return ghostForWaypt(c, wpt);
}

static naRef f_createWPFrom(naContext c, naRef me, int argc, naRef* args)
{
  if (argc < 1) {
    naRuntimeError(c, "createWPFrom: need at least one argument");
  }

  FGPositioned* positioned = positionedGhost(args[0]);
  if (!positioned) {
    naRuntimeError(c, "createWPFrom: couldn't convert arg[0] to FGPositioned");
  }

  WayptRef wpt;
  if (positioned->type() == FGPositioned::RUNWAY) {
    wpt = new RunwayWaypt((FGRunway*) positioned, NULL);
  } else {
    wpt = new NavaidWaypoint(positioned, NULL);
  }

  // set waypt flags - approach, departure, pseudo, etc
  if (argc > 1) {
    WayptFlag f = wayptFlagFromString(naStr_data(args[1]));
    if (f == 0) {
      naRuntimeError(c, "createWPFrom: bad waypoint role");
    }
    wpt->setFlag(f);
  }

  return ghostForWaypt(c, wpt);
}

static naRef f_createViaTo(naContext c, naRef me, int argc, naRef* args)
{
    if (argc != 2) {
        naRuntimeError(c, "createViaTo: needs exactly two arguments");
    }

    std::string airwayName = naStr_data(args[0]);
    AirwayRef airway = Airway::findByIdent(airwayName, Airway::Both);
    if (!airway) {
        naRuntimeError(c, "createViaTo: couldn't find airway with provided name: %s", 
          naStr_data(args[0]));
    }

    FGPositionedRef nav;
    if (naIsString(args[1])) {
        WayptRef enroute = airway->findEnroute(naStr_data(args[1]));
        if (!enroute) {
            naRuntimeError(c, "unknown waypoint on airway %s: %s",
                           naStr_data(args[0]), naStr_data(args[1]));
        }

        nav = enroute->source();
    } else {
        nav = positionedGhost(args[1]);
        if (!nav) {
            naRuntimeError(c, "createViaTo: arg[1] is not a navaid");
        }
    }

    if (!airway->containsNavaid(nav)) {
        naRuntimeError(c, "createViaTo: navaid not on airway");
    }

    Via* via = new Via(nullptr, airway, nav);
    return ghostForWaypt(c, via);
}

static naRef f_createViaFromTo(naContext c, naRef me, int argc, naRef* args)
{
    if (argc != 3) {
        naRuntimeError(c, "createViaFromTo: needs exactly three arguments");
    }

    auto from = positionedFromArg(args[0]);
    if (!from) {
      naRuntimeError(c, "createViaFromTo: from wp not found");
    }

    std::string airwayName = naStr_data(args[1]);
    AirwayRef airway = Airway::findByIdentAndNavaid(airwayName, from);
    if (!airway) {
        naRuntimeError(c, "createViaFromTo: couldn't find airway with provided name: %s from wp %s", 
          naStr_data(args[0]), 
          from->ident().c_str());
    }

    FGPositionedRef nav;
    if (naIsString(args[2])) {
        WayptRef enroute = airway->findEnroute(naStr_data(args[2]));
        if (!enroute) {
            naRuntimeError(c, "unknown waypoint on airway %s: %s",
                           naStr_data(args[1]), naStr_data(args[2]));
        }

        nav = enroute->source();
    } else {
        nav = positionedFromArg(args[2]);
        if (!nav) {
            naRuntimeError(c, "createViaFromTo: arg[2] is not a navaid");
        }
    }

    if (!airway->containsNavaid(nav)) {
        naRuntimeError(c, "createViaFromTo: navaid not on airway");
    }

    Via* via = new Via(nullptr, airway, nav);
    return ghostForWaypt(c, via);
}

static naRef f_createDiscontinuity(naContext c, naRef me, int argc, naRef* args)
{
    return ghostForWaypt(c, new Discontinuity(NULL));
}

static naRef f_flightplan_getWP(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.getWP called on non-flightplan object");
  }

  int index;
  if (argc == 0) {
    index = fp->currentIndex();
  } else {
    index = (int) naNumValue(args[0]).num;
  }

  if ((index < 0) || (index >= fp->numLegs())) {
    return naNil();
  }

  return ghostForLeg(c, fp->legAtIndex(index));
}

static naRef f_flightplan_currentWP(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.currentWP called on non-flightplan object");
  }
  return ghostForLeg(c, fp->currentLeg());
}

static naRef f_flightplan_nextWP(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.nextWP called on non-flightplan object");
  }
  return ghostForLeg(c, fp->nextLeg());
}

static naRef f_flightplan_numWaypoints(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.numWaypoints called on non-flightplan object");
  }
  return naNum(fp->numLegs());
}

static naRef f_flightplan_appendWP(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.appendWP called on non-flightplan object");
  }

  WayptRef wp = wayptGhost(args[0]);
  int index = fp->numLegs();
  fp->insertWayptAtIndex(wp.get(), index);
  return naNum(index);
}

static naRef f_flightplan_insertWP(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.insertWP called on non-flightplan object");
  }

  WayptRef wp = wayptGhost(args[0]);
  int index = -1; // append
  if ((argc > 1) && naIsNum(args[1])) {
    index = (int) args[1].num;
  }

  auto leg = fp->insertWayptAtIndex(wp.get(), index);
  return ghostForLeg(c, leg);
}

static naRef f_flightplan_insertWPAfter(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.insertWPAfter called on non-flightplan object");
  }

  WayptRef wp = wayptGhost(args[0]);
  int index = -1; // append
  if ((argc > 1) && naIsNum(args[1])) {
    index = (int) args[1].num;
  }

  auto leg = fp->insertWayptAtIndex(wp.get(), index + 1);
  return ghostForLeg(c, leg);
}

static naRef f_flightplan_insertWaypoints(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.insertWaypoints called on non-flightplan object");
  }

  // don't warn when passing a nil to this, which can happen in certain
  // procedure construction situations
  if (naIsNil(args[0])) {
    return naNil();
  }

  WayptVec wps;
  if (!naIsVector(args[0])) {
    naRuntimeError(c, "flightplan.insertWaypoints expects vector as first arg");
  }

  int count = naVec_size(args[0]);
  for (int i=0; i<count; ++i) {
    Waypt* wp = wayptGhost(naVec_get(args[0], i));
    if (wp) {
      wps.push_back(wp);
    }
  }

  int index = -1; // append
  if ((argc > 1) && naIsNum(args[1])) {
    index = (int) args[1].num;
  }

  fp->insertWayptsAtIndex(wps, index);
  return naNil();
}

static naRef f_flightplan_deleteWP(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.deleteWP called on non-flightplan object");
  }

  if ((argc < 1) || !naIsNum(args[0])) {
    naRuntimeError(c, "bad argument to flightplan.deleteWP");
  }

  int index = (int) args[0].num;
  fp->deleteIndex(index);
  return naNil();
}

static naRef f_flightplan_clearPlan(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.clearPlan called on non-flightplan object");
  }

  fp->clear();
  return naNil();
}

static naRef f_flightplan_clearWPType(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.clearWPType called on non-flightplan object");
  }

  if (argc < 1) {
    naRuntimeError(c, "insufficent args to flightplan.clearWPType");
  }

  WayptFlag flag = wayptFlagFromString(naStr_data(args[0]));
  if (flag == 0) {
    naRuntimeError(c, "clearWPType: bad waypoint role");
  }

  fp->clearWayptsWithFlag(flag);
  return naNil();
}

static naRef f_flightplan_clone(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.clone called on non-flightplan object");
  }

  return ghostForFlightPlan(c, fp->clone());
}

static naRef f_flightplan_pathGeod(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.clone called on non-flightplan object");
  }

  if ((argc < 1) || !naIsNum(args[0])) {
    naRuntimeError(c, "bad argument to flightplan.pathGeod");
  }

  if ((argc > 1) && !naIsNum(args[1])) {
    naRuntimeError(c, "bad argument to flightplan.pathGeod");
  }

  int index = (int) args[0].num;
  double offset = (argc > 1) ? args[1].num : 0.0;
  naRef result = naNewHash(c);
  SGGeod g = fp->pointAlongRoute(index, offset);
  hashset(c, result, "lat", naNum(g.getLatitudeDeg()));
  hashset(c, result, "lon", naNum(g.getLongitudeDeg()));
  return result;
}

static naRef f_flightplan_finish(naContext c, naRef me, int argc, naRef* args)
{
    FlightPlan* fp = flightplanGhost(me);
    if (!fp) {
        naRuntimeError(c, "flightplan.finish called on non-flightplan object");
    }

    fp->finish();
    return naNil();
}

static naRef f_flightplan_activate(naContext c, naRef me, int argc, naRef* args)
{
    FlightPlan* fp = flightplanGhost(me);
    if (!fp) {
        naRuntimeError(c, "activate called on non-flightplan object");
    }

    fp->activate();
    return naNil();
}


static naRef f_flightplan_indexOfWp(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "flightplan.indexOfWP called on non-flightplan object");
  }

  FGPositioned* positioned = positionedGhost(args[0]);
  if (positioned) {
    return naNum(fp->findWayptIndex(positioned));
  }

  FlightPlan::Leg* leg = fpLegGhost(args[0]);
  if (leg) {
    if (leg->owner() == fp) {
      return naNum(leg->index());
    }

    naRuntimeError(c, "flightplan.indexOfWP called on leg from different flightplan");
  }

  SGGeod pos;
  int argOffset = geodFromArgs(args, 0, argc, pos);
  if (argOffset > 0) {
    return naNum(fp->findWayptIndex(pos));
  }

  return naNum(-1);
}

static naRef f_flightplan_save(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "save called on non-flightplan object");
  }

  if ((argc < 1) || !naIsString(args[0])) {
    naRuntimeError(c, "flightplan.save, no file path argument");
  }

  SGPath raw_path(naStr_data(args[0]));
  SGPath validated_path = fgValidatePath(raw_path, true);
  if (validated_path.isNull()) {
    naRuntimeError(c, "flightplan.save, writing to path is not permitted");
  }

  bool ok = fp->save(validated_path);
  return naNum(ok);
}

static naRef f_flightplan_parseICAORoute(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "parseICAORoute called on non-flightplan object");
  }

  if ((argc < 1) || !naIsString(args[0])) {
    naRuntimeError(c, "flightplan.parseICAORoute, no route argument");
  }

  bool ok = fp->parseICAORouteString(naStr_data(args[0]));
  return naNum(ok);
}

static naRef f_flightplan_toICAORoute(naContext c, naRef me, int, naRef*)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "toICAORoute called on non-flightplan object");
  }

  return stringToNasal(c, fp->asICAORouteString());
}

static naRef f_flightplan_computeDuration(naContext c, naRef me, int, naRef*)
{
  FlightPlan* fp = flightplanGhost(me);
  if (!fp) {
    naRuntimeError(c, "computeDuration called on non-flightplan object");
  }

  fp->computeDurationMinutes();
  return naNum(fp->estimatedDurationMinutes());
}

static naRef f_leg_setSpeed(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan::Leg* leg = fpLegGhost(me);
  if (!leg) {
    naRuntimeError(c, "leg.setSpeed called on non-flightplan-leg object");
  }

  double speed = 0.0;
  RouteRestriction rr = RESTRICT_AT;
  if (argc > 0) {
    if (naIsNil(args[0])) {
        // clear the restriction to NONE
      rr = RESTRICT_NONE;
    } else if (convertToNum(args[0], speed)) {
      if ((argc > 1) && naIsString(args[1])) {
        rr = routeRestrictionFromArg(args[1]);
      } else {
        naRuntimeError(c, "bad arguments to setSpeed");
      }
    }

    leg->setSpeed(rr, speed);
  } else {
      naRuntimeError(c, "bad arguments to setSpeed");
  }

  return naNil();
}

static naRef f_leg_setAltitude(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan::Leg* leg = fpLegGhost(me);
  if (!leg) {
    naRuntimeError(c, "leg.setAltitude called on non-flightplan-leg object");
  }

  double altitude = 0.0;
  RouteRestriction rr = RESTRICT_AT;
  if (argc > 0) {
    if (naIsNil(args[0])) {
      // clear the restriction to NONE
      rr = RESTRICT_NONE;
    } else if (convertToNum(args[0], altitude)) {
      if (argc > 1) {
        rr = routeRestrictionFromArg(args[1]);
      } else {
        naRuntimeError(c, "bad arguments to leg.setAltitude");
      }
    }

    leg->setAltitude(rr, altitude);
  } else {
      naRuntimeError(c, "bad arguments to setleg.setAltitude");
  }

  return naNil();
}

static naRef f_leg_path(naContext c, naRef me, int argc, naRef* args)
{
  FlightPlan::Leg* leg = fpLegGhost(me);
  if (!leg) {
    naRuntimeError(c, "leg.setAltitude called on non-flightplan-leg object");
  }

  RoutePath path(leg->owner());
  SGGeodVec gv(path.pathForIndex(leg->index()));

  naRef result = naNewVector(c);
  for (SGGeod p : gv) {
    // construct a geo.Coord!
    naRef coord = naNewHash(c);
    hashset(c, coord, "lat", naNum(p.getLatitudeDeg()));
    hashset(c, coord, "lon", naNum(p.getLongitudeDeg()));
    naVec_append(result, coord);
  }

  return result;
}

static naRef f_leg_courseAndDistanceFrom(naContext c, naRef me, int argc, naRef* args)
{
    FlightPlan::Leg* leg = fpLegGhost(me);
    if (!leg) {
        naRuntimeError(c, "leg.courseAndDistanceFrom called on non-flightplan-leg object");
    }

    SGGeod pos;
    geodFromArgs(args, 0, argc, pos);

    RoutePath path(leg->owner());
    SGGeod wpPos = path.positionForIndex(leg->index());
    double courseDeg, az2, distanceM;
    SGGeodesy::inverse(pos, wpPos, courseDeg, az2, distanceM);

    naRef result = naNewVector(c);
    naVec_append(result, naNum(courseDeg));
    naVec_append(result, naNum(distanceM * SG_METER_TO_NM));
    return result;
}

static naRef f_procedure_transition(naContext c, naRef me, int argc, naRef* args)
{
  Procedure* proc = procedureGhost(me);
  if (!proc) {
    naRuntimeError(c, "procedure.transition called on non-procedure object");
  }

  if ((proc->type() != PROCEDURE_SID) && (proc->type() != PROCEDURE_STAR)) {
    naRuntimeError(c, "procedure.transition called on non-SID or -STAR");
  }

  ArrivalDeparture* ad = (ArrivalDeparture*) proc;
  Transition* trans = ad->findTransitionByName(naStr_data(args[0]));

  return ghostForProcedure(c, trans);
}

static naRef f_procedure_route(naContext c, naRef me, int argc, naRef* args)
{
  Procedure* proc = procedureGhost(me);
  if (!proc) {
    naRuntimeError(c, "procedure.route called on non-procedure object");
  }

// wrapping up tow different routines here - approach routing from the IAF
// to the associated runway, and SID/STAR routing via an enroute transition
// and possibly a runway transition or not.
  if (Approach::isApproach(proc->type())) {
    WayptRef iaf;
    if (argc > 0) {
      iaf = wayptFromArg(args[0]);
    }

    WayptVec r;
    Approach* app = (Approach*) proc;
    if (!app->route(iaf, r)) {
      return naNil();
    }

    return convertWayptVecToNasal(c, r);
  } else if ((proc->type() != PROCEDURE_SID) && (proc->type() != PROCEDURE_STAR)) {
    naRuntimeError(c, "procedure.route called on unsuitable procedure type");
  }

  int argOffset = 0;
  FGRunway* rwy = runwayGhost(args[0]);
  if (rwy) ++argOffset;

  ArrivalDeparture* ad = (ArrivalDeparture*) proc;
  Transition* trans = NULL;
  if (argOffset < argc) {
    trans = (Transition*) procedureGhost(args[argOffset]);
  }

  // note either runway or trans may be NULL - that's ok
  WayptVec r;
  if (!ad->route(rwy, trans, r)) {
    SG_LOG(SG_NASAL, SG_WARN, "procedure.route failed for ArrivalDeparture somehow");
    return naNil();
  }

  return convertWayptVecToNasal(c, r);
}

static naRef f_airway_contains(naContext c, naRef me, int argc, naRef* args)
{
  Airway* awy = airwayGhost(me);
  if (!awy) {
    naRuntimeError(c, "airway.contains called on non-airway object");
  }

  if (argc < 1) {
    naRuntimeError(c, "missing arg to airway.contains");
  }

  auto pos = positionedFromArg(args[0]);
  if (!pos) {
    return naNum(0);
  }

  return naNum(awy->containsNavaid(pos));
}

// Table of extension functions.  Terminate with zeros.
static struct { const char* name; naCFunction func; } funcs[] = {
  { "carttogeod", f_carttogeod },
  { "geodtocart", f_geodtocart },
  { "geodinfo", f_geodinfo },
  { "formatLatLon", f_formatLatLon },
  { "parseStringAsLatLonValue", f_parseStringAsLatLonValue},
  { "get_cart_ground_intersection", f_get_cart_ground_intersection },
  { "aircraftToCart", f_aircraftToCart },
  { "airportinfo", f_airportinfo },
  { "findAirportsWithinRange", f_findAirportsWithinRange },
  { "findAirportsByICAO", f_findAirportsByICAO },
  { "navinfo", f_navinfo },
  { "findNavaidsWithinRange", f_findNavaidsWithinRange },
  { "findNDBByFrequencyKHz", f_findNDBByFrequency },
  { "findNDBsByFrequencyKHz", f_findNDBsByFrequency },
  { "findNavaidByFrequencyMHz", f_findNavaidByFrequency },
  { "findNavaidsByFrequencyMHz", f_findNavaidsByFrequency },
  { "findNavaidsByID", f_findNavaidsByIdent },
  { "findFixesByID", f_findFixesByIdent },
  { "findByIdent", f_findByIdent },
  { "flightplan", f_flightplan },
  { "createFlightplan", f_createFlightplan },
  { "registerFlightPlanDelegate", f_registerFPDelegate },
  { "createWP", f_createWP },
  { "createWPFrom", f_createWPFrom },
  { "createViaTo", f_createViaTo },
  { "createViaFromTo", f_createViaFromTo },
  { "createDiscontinuity", f_createDiscontinuity },
  { "airwaysRoute", f_airwaySearch },
  { "airway", f_findAirway },
  { "magvar", f_magvar },
  { "courseAndDistance", f_courseAndDistance },
  { "greatCircleMove", f_greatCircleMove },
  { "tileIndex", f_tileIndex },
  { "tilePath", f_tilePath },
  { 0, 0 }
};


naRef initNasalPositioned(naRef globals, naContext c)
{
    airportPrototype = naNewHash(c);
    naSave(c, airportPrototype);

    hashset(c, airportPrototype, "runway", naNewFunc(c, naNewCCode(c, f_airport_runway)));
    hashset(c, airportPrototype, "runwaysWithoutReciprocals", naNewFunc(c, naNewCCode(c, f_airport_runwaysWithoutReciprocals)));
    hashset(c, airportPrototype, "helipad", naNewFunc(c, naNewCCode(c, f_airport_runway)));
    hashset(c, airportPrototype, "tower", naNewFunc(c, naNewCCode(c, f_airport_tower)));
    hashset(c, airportPrototype, "comms", naNewFunc(c, naNewCCode(c, f_airport_comms)));
    hashset(c, airportPrototype, "sids", naNewFunc(c, naNewCCode(c, f_airport_sids)));
    hashset(c, airportPrototype, "stars", naNewFunc(c, naNewCCode(c, f_airport_stars)));
    hashset(c, airportPrototype, "getApproachList", naNewFunc(c, naNewCCode(c, f_airport_approaches)));
    hashset(c, airportPrototype, "parking", naNewFunc(c, naNewCCode(c, f_airport_parking)));
    hashset(c, airportPrototype, "getSid", naNewFunc(c, naNewCCode(c, f_airport_getSid)));
    hashset(c, airportPrototype, "getStar", naNewFunc(c, naNewCCode(c, f_airport_getStar)));
    hashset(c, airportPrototype, "getIAP", naNewFunc(c, naNewCCode(c, f_airport_getApproach)));
    hashset(c, airportPrototype, "findBestRunwayForPos", naNewFunc(c, naNewCCode(c, f_airport_findBestRunway)));
    hashset(c, airportPrototype, "tostring", naNewFunc(c, naNewCCode(c, f_airport_toString)));

    flightplanPrototype = naNewHash(c);
    naSave(c, flightplanPrototype);

    hashset(c, flightplanPrototype, "getWP", naNewFunc(c, naNewCCode(c, f_flightplan_getWP)));
    hashset(c, flightplanPrototype, "currentWP", naNewFunc(c, naNewCCode(c, f_flightplan_currentWP)));
    hashset(c, flightplanPrototype, "nextWP", naNewFunc(c, naNewCCode(c, f_flightplan_nextWP)));
    hashset(c, flightplanPrototype, "getPlanSize", naNewFunc(c, naNewCCode(c, f_flightplan_numWaypoints)));
    // alias to this name also
    hashset(c, flightplanPrototype, "numWaypoints", naNewFunc(c, naNewCCode(c, f_flightplan_numWaypoints)));
    hashset(c, flightplanPrototype, "appendWP", naNewFunc(c, naNewCCode(c, f_flightplan_appendWP)));
    hashset(c, flightplanPrototype, "insertWP", naNewFunc(c, naNewCCode(c, f_flightplan_insertWP)));
    hashset(c, flightplanPrototype, "deleteWP", naNewFunc(c, naNewCCode(c, f_flightplan_deleteWP)));
    hashset(c, flightplanPrototype, "insertWPAfter", naNewFunc(c, naNewCCode(c, f_flightplan_insertWPAfter)));
    hashset(c, flightplanPrototype, "insertWaypoints", naNewFunc(c, naNewCCode(c, f_flightplan_insertWaypoints)));
    hashset(c, flightplanPrototype, "cleanPlan", naNewFunc(c, naNewCCode(c, f_flightplan_clearPlan)));
    hashset(c, flightplanPrototype, "clearWPType", naNewFunc(c, naNewCCode(c, f_flightplan_clearWPType)));
    hashset(c, flightplanPrototype, "clone", naNewFunc(c, naNewCCode(c, f_flightplan_clone)));
    hashset(c, flightplanPrototype, "pathGeod", naNewFunc(c, naNewCCode(c, f_flightplan_pathGeod)));
    hashset(c, flightplanPrototype, "finish", naNewFunc(c, naNewCCode(c, f_flightplan_finish)));
    hashset(c, flightplanPrototype, "activate", naNewFunc(c, naNewCCode(c, f_flightplan_activate)));
    hashset(c, flightplanPrototype, "indexOfWP", naNewFunc(c, naNewCCode(c, f_flightplan_indexOfWp)));
    hashset(c, flightplanPrototype, "computeDuration", naNewFunc(c, naNewCCode(c, f_flightplan_computeDuration)));
    hashset(c, flightplanPrototype, "parseICAORoute", naNewFunc(c, naNewCCode(c, f_flightplan_parseICAORoute)));
    hashset(c, flightplanPrototype, "toICAORoute", naNewFunc(c, naNewCCode(c, f_flightplan_toICAORoute)));

    hashset(c, flightplanPrototype, "save", naNewFunc(c, naNewCCode(c, f_flightplan_save)));

    procedurePrototype = naNewHash(c);
    naSave(c, procedurePrototype);
    hashset(c, procedurePrototype, "transition", naNewFunc(c, naNewCCode(c, f_procedure_transition)));
    hashset(c, procedurePrototype, "route", naNewFunc(c, naNewCCode(c, f_procedure_route)));

    fpLegPrototype = naNewHash(c);
    naSave(c, fpLegPrototype);
    hashset(c, fpLegPrototype, "setSpeed", naNewFunc(c, naNewCCode(c, f_leg_setSpeed)));
    hashset(c, fpLegPrototype, "setAltitude", naNewFunc(c, naNewCCode(c, f_leg_setAltitude)));
    hashset(c, fpLegPrototype, "path", naNewFunc(c, naNewCCode(c, f_leg_path)));
    hashset(c, fpLegPrototype, "courseAndDistanceFrom", naNewFunc(c, naNewCCode(c, f_leg_courseAndDistanceFrom)));

    airwayPrototype = naNewHash(c);
    naSave(c, airwayPrototype);
    hashset(c, airwayPrototype, "contains", naNewFunc(c, naNewCCode(c, f_airway_contains)));

    for(int i=0; funcs[i].name; i++) {
      hashset(c, globals, funcs[i].name,
      naNewFunc(c, naNewCCode(c, funcs[i].func)));
    }

  return naNil();
}

void postinitNasalPositioned(naRef globals, naContext c)
{
  naRef geoModule = naHash_cget(globals, (char*) "geo");
  if (naIsNil(geoModule)) {
    SG_LOG(SG_GENERAL, SG_WARN, "postinitNasalPositioned: geo.nas not loaded");
    return;
  }

  geoCoordClass = naHash_cget(geoModule, (char*) "Coord");
}
