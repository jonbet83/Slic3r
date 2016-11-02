#include <assert.h>
#include <stdio.h>

#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../Surface.hpp"

#include "FillBase.hpp"

namespace Slic3r {

// Generate infills for Slic3r::Layer::Region.
// The Slic3r::Layer::Region at this point of time may contain
// surfaces of various types (internal/bridge/top/bottom/solid).
// The infills are generated on the groups of surfaces with a compatible type. 
// Returns an array of Slic3r::ExtrusionPath::Collection objects containing the infills generaed now
// and the thin fills generated by generate_perimeters().
void make_fill(LayerRegion &layerm, ExtrusionEntityCollection &out)
{    
//    Slic3r::debugf "Filling layer %d:\n", $layerm->layer->id;
    
    double  fill_density           = layerm.region()->config.fill_density;
    Flow    infill_flow            = layerm.flow(frInfill);
    Flow    solid_infill_flow      = layerm.flow(frSolidInfill);
    Flow    top_solid_infill_flow  = layerm.flow(frTopSolidInfill);

    Surfaces surfaces;
    
    // merge adjacent surfaces
    // in case of bridge surfaces, the ones with defined angle will be attached to the ones
    // without any angle (shouldn't this logic be moved to process_external_surfaces()?)
    {
        SurfacesPtr surfaces_with_bridge_angle;
        surfaces_with_bridge_angle.reserve(layerm.fill_surfaces.surfaces.size());
        for (Surfaces::iterator it = layerm.fill_surfaces.surfaces.begin(); it != layerm.fill_surfaces.surfaces.end(); ++ it)
            if (it->bridge_angle >= 0)
                surfaces_with_bridge_angle.push_back(&(*it));
        
        // group surfaces by distinct properties (equal surface_type, thickness, thickness_layers, bridge_angle)
        // group is of type Slic3r::SurfaceCollection
        //FIXME: Use some smart heuristics to merge similar surfaces to eliminate tiny regions.
        std::vector<SurfacesPtr> groups;
        layerm.fill_surfaces.group(&groups);
        
        // merge compatible groups (we can generate continuous infill for them)
        {
            // cache flow widths and patterns used for all solid groups
            // (we'll use them for comparing compatible groups)
            std::vector<char>   is_solid(groups.size(), false);
            std::vector<float>  fw(groups.size(), 0.f);
            std::vector<int>    pattern(groups.size(), -1);
            for (size_t i = 0; i < groups.size(); ++ i) {
                // we can only merge solid non-bridge surfaces, so discard
                // non-solid surfaces
                const Surface &surface = *groups[i].front();
                if (surface.is_solid() && (!surface.is_bridge() || layerm.layer()->id() == 0)) {
                    is_solid[i] = true;
                    fw[i] = (surface.surface_type == stTop) ? top_solid_infill_flow.width : solid_infill_flow.width;
                    pattern[i] = surface.is_external() ? layerm.region()->config.external_fill_pattern.value : ipRectilinear;
                }
            }
            // loop through solid groups
            for (size_t i = 0; i < groups.size(); ++ i) {
                if (is_solid[i]) {
                    // find compatible groups and append them to this one
                    for (size_t j = i + 1; j < groups.size(); ++ j) {
                        if (is_solid[j] && fw[i] == fw[j] && pattern[i] == pattern[j]) {
                            // groups are compatible, merge them
                            groups[i].insert(groups[i].end(), groups[j].begin(), groups[j].end());
                            groups.erase(groups.begin() + j);
                            is_solid.erase(is_solid.begin() + j);
                            fw.erase(fw.begin() + j);
                            pattern.erase(pattern.begin() + j);
                        }
                    }
                }
            }
        }
        
        // Give priority to bridges. Process the bridges in the first round, the rest of the surfaces in the 2nd round.
        for (size_t round = 0; round < 2; ++ round) {
            for (std::vector<SurfacesPtr>::iterator it_group = groups.begin(); it_group != groups.end(); ++ it_group) {
                const SurfacesPtr &group = *it_group;
                bool is_bridge = group.front()->bridge_angle >= 0;
                if (is_bridge != (round == 0))
                    continue;
                // Make a union of polygons defining the infiill regions of a group, use a safety offset.
                Polygons union_p = union_(to_polygons(*it_group), true);
                // Subtract surfaces having a defined bridge_angle from any other, use a safety offset.
                if (! surfaces_with_bridge_angle.empty() && it_group->front()->bridge_angle < 0)
                    union_p = diff(union_p, to_polygons(surfaces_with_bridge_angle), true);
                // subtract any other surface already processed
                //FIXME Vojtech: Because the bridge surfaces came first, they are subtracted twice!
                ExPolygons union_expolys = diff_ex(union_p, to_polygons(surfaces), true);
                for (ExPolygons::const_iterator it_expoly = union_expolys.begin(); it_expoly != union_expolys.end(); ++ it_expoly)
                    surfaces.push_back(Surface(*it_group->front(), *it_expoly));
            }
        }
    }
    
    // we need to detect any narrow surfaces that might collapse
    // when adding spacing below
    // such narrow surfaces are often generated in sloping walls
    // by bridge_over_infill() and combine_infill() as a result of the
    // subtraction of the combinable area from the layer infill area,
    // which leaves small areas near the perimeters
    // we are going to grow such regions by overlapping them with the void (if any)
    // TODO: detect and investigate whether there could be narrow regions without
    // any void neighbors
    {
        coord_t distance_between_surfaces = std::max(
            std::max(infill_flow.scaled_spacing(), solid_infill_flow.scaled_spacing()),
            top_solid_infill_flow.scaled_spacing());
        Polygons surfaces_polygons = to_polygons(surfaces);
        Polygons collapsed = diff(
            surfaces_polygons,
            offset2(surfaces_polygons, -distance_between_surfaces/2, +distance_between_surfaces/2),
            true);
        Polygons to_subtract;
        to_subtract.reserve(collapsed.size() + number_polygons(surfaces));
        for (Surfaces::const_iterator it_surface = surfaces.begin(); it_surface != surfaces.end(); ++ it_surface)
            if (it_surface->surface_type == stInternalVoid)
                polygons_append(to_subtract, *it_surface);
        polygons_append(to_subtract, collapsed);
        surfaces_append(
            surfaces,
            intersection_ex(
                offset(collapsed, distance_between_surfaces),
                to_subtract,
                true),
            stInternalSolid);
    }

    if (0) {
//        require "Slic3r/SVG.pm";
//        Slic3r::SVG::output("fill_" . $layerm->print_z . ".svg",
//            expolygons      => [ map $_->expolygon, grep !$_->is_solid, @surfaces ],
//            red_expolygons  => [ map $_->expolygon, grep  $_->is_solid, @surfaces ],
//        );
    }

    for (Surfaces::const_iterator surface_it = surfaces.begin(); surface_it != surfaces.end(); ++ surface_it) {
        const Surface &surface = *surface_it;
        if (surface.surface_type == stInternalVoid)
            continue;
        InfillPattern  fill_pattern = layerm.region()->config.fill_pattern.value;
        double         density      = fill_density;
        FlowRole role = (surface.surface_type == stTop) ? frTopSolidInfill :
            (surface.is_solid() ? frSolidInfill : frInfill);
        bool is_bridge = layerm.layer()->id() > 0 && surface.is_bridge();
        
        if (surface.is_solid()) {
            density = 100;
            fill_pattern = (surface.is_external() && ! is_bridge) ? 
                layerm.region()->config.external_fill_pattern.value :
                ipRectilinear;
        } else if (density <= 0)
            continue;
        
        // get filler object
        std::auto_ptr<Fill> f = std::auto_ptr<Fill>(Fill::new_from_type(fill_pattern));
        f->set_bounding_box(layerm.layer()->object()->bounding_box());
        
        // calculate the actual flow we'll be using for this infill
        coordf_t h = (surface.thickness == -1) ? layerm.layer()->height : surface.thickness;
        Flow flow = layerm.region()->flow(
            role,
            h,
            is_bridge || f->use_bridge_flow(),  // bridge flow?
            layerm.layer()->id() == 0,          // first layer?
            -1,                                 // auto width
            *layerm.layer()->object()
        );
        
        // calculate flow spacing for infill pattern generation
        bool using_internal_flow = false;
        if (! surface.is_solid() && ! is_bridge) {
            // it's internal infill, so we can calculate a generic flow spacing 
            // for all layers, for avoiding the ugly effect of
            // misaligned infill on first layer because of different extrusion width and
            // layer height
            Flow internal_flow = layerm.region()->flow(
                frInfill,
                layerm.layer()->object()->config.layer_height.value,  // TODO: handle infill_every_layers?
                false,  // no bridge
                false,  // no first layer
                -1,     // auto width
                *layerm.layer()->object()
            );
            f->spacing = internal_flow.spacing();
            using_internal_flow = 1;
        } else {
            f->spacing = flow.spacing();
        }

        double link_max_length = 0.;
        if (! is_bridge) {
            link_max_length = layerm.region()->config.get_abs_value(surface.is_external() ? "external_fill_link_max_length" : "fill_link_max_length", flow.spacing());
//            printf("flow spacing: %f,  is_external: %d, link_max_length: %lf\n", flow.spacing(), int(surface.is_external()), link_max_length);
        }
        
        f->layer_id = layerm.layer()->id();
        f->z = layerm.layer()->print_z;
        f->angle = Geometry::deg2rad(layerm.region()->config.fill_angle.value);
        // Maximum length of the perimeter segment linking two infill lines.
        f->link_max_length = scale_(link_max_length);
        // Used by the concentric infill pattern to clip the loops to create extrusion paths.
        f->loop_clipping = scale_(flow.nozzle_diameter) * LOOP_CLIPPING_LENGTH_OVER_NOZZLE_DIAMETER;
//        f->layer_height = h;

        // apply half spacing using this flow's own spacing and generate infill
        FillParams params;
        params.density = 0.01 * density;
        params.dont_adjust = true;
        Polylines polylines = f->fill_surface(&surface, params);
        if (polylines.empty())
            continue;

        // calculate actual flow from spacing (which might have been adjusted by the infill
        // pattern generator)
        if (using_internal_flow) {
            // if we used the internal flow we're not doing a solid infill
            // so we can safely ignore the slight variation that might have
            // been applied to $f->flow_spacing
        } else {
            flow = Flow::new_from_spacing(f->spacing, flow.nozzle_diameter, h, is_bridge || f->use_bridge_flow());
        }

        // save into layer
        {
            ExtrusionRole role = is_bridge ? erBridgeInfill :
                (surface.is_solid() ? ((surface.surface_type == stTop) ? erTopSolidInfill : erSolidInfill) : erInternalInfill);
            ExtrusionEntityCollection &collection = *(new ExtrusionEntityCollection());
            out.entities.push_back(&collection);
            // Only concentric fills are not sorted.
            collection.no_sort = f->no_sort();
            for (Polylines::iterator it = polylines.begin(); it != polylines.end(); ++ it) {
                ExtrusionPath *path = new ExtrusionPath(role);
                collection.entities.push_back(path);
                path->polyline.points.swap(it->points);
                path->mm3_per_mm = flow.mm3_per_mm();
                path->width      = flow.width,
                path->height     = flow.height;
            }
        }
    }
    
    // add thin fill regions
    // thin_fills are of C++ Slic3r::ExtrusionEntityCollection, perl type Slic3r::ExtrusionPath::Collection
    // Unpacks the collection, creates multiple collections per path.
    // The path type could be ExtrusionPath, ExtrusionLoop or ExtrusionEntityCollection.
    // Why the paths are unpacked?
    for (ExtrusionEntitiesPtr::iterator thin_fill = layerm.thin_fills.entities.begin(); thin_fill != layerm.thin_fills.entities.end(); ++ thin_fill) {
    #if 0
        out.entities.push_back((*thin_fill)->clone());
        assert(dynamic_cast<ExtrusionEntityCollection*>(out.entities.back()) != NULL);
    #else
        ExtrusionEntityCollection &collection = *(new ExtrusionEntityCollection());
        out.entities.push_back(&collection);
        collection.entities.push_back((*thin_fill)->clone());
    #endif
    }
}

} // namespace Slic3r