// Portions of this file are Copyright 2023 Google LLC, and licensed under GPL2+. See COPYING.
#ifdef ENABLE_MANIFOLD

#include "cgal.h"
#include "cgalutils.h"
#include <CGAL/convex_hull_3.h>

#include "PolySet.h"
#include "printutils.h"
#include "manifoldutils.h"
#include "ManifoldGeometry.h"
#include "parallel.h"
#include "node.h"
#include "PolySetUtils.h"

namespace ManifoldUtils {

namespace {


std::shared_ptr<ManifoldGeometry> minkowskiOp(const ManifoldGeometry& lhs, const ManifoldGeometry& rhs) {
// FIXME: How to deal with operation not supported?
#ifdef ENABLE_CGAL
  auto lhs_nef = std::shared_ptr<CGAL_Nef_polyhedron>(CGALUtils::createNefPolyhedronFromPolySet(*lhs.toPolySet()));
  auto rhs_nef = std::shared_ptr<CGAL_Nef_polyhedron>(CGALUtils::createNefPolyhedronFromPolySet(*rhs.toPolySet()));
  if (lhs_nef->isEmpty() || rhs_nef->isEmpty()) {
    return {};
  }
  lhs_nef->minkowski(*rhs_nef);

  auto ps = PolySetUtils::getGeometryAsPolySet(lhs_nef);
  if (!ps) return {};
  else {
    return ManifoldUtils::createManifoldFromPolySet(*ps);
  }
#endif
}

std::shared_ptr<const Geometry> applyPairwiseMinkowski(const Geometry::Geometries& children)
{
  std::shared_ptr<ManifoldGeometry> geom;

  for (const auto& item : children) {
    auto child_geom = item.second ? createManifoldFromGeometry(item.second) : nullptr;
    if (!child_geom || child_geom->isEmpty()) continue;

    if (!geom) {
      geom = std::make_shared<ManifoldGeometry>(*child_geom);
    } else {
      geom = minkowskiOp(*geom, *child_geom);
    }
    if (item.first) item.first->progress_report();
  }
  return geom;
}


#ifdef ENABLE_CGAL
template <typename Polyhedron>
class CGALPolyhedronBuilderFromManifold : public CGAL::Modifier_base<typename Polyhedron::HalfedgeDS>
{
  using HDS = typename Polyhedron::HalfedgeDS;
  using CGAL_Polybuilder = CGAL::Polyhedron_incremental_builder_3<typename Polyhedron::HalfedgeDS>;
public:
  using CGALPoint = typename CGAL_Polybuilder::Point_3;

  const manifold::Mesh& mesh;
  CGALPolyhedronBuilderFromManifold(const manifold::Mesh& mesh) : mesh(mesh) { }

  void operator()(HDS& hds) override {
    CGAL_Polybuilder B(hds, true);
  
    B.begin_surface(mesh.vertPos.size(), mesh.triVerts.size());
    for (const auto &v : mesh.vertPos) {
      B.add_vertex(CGALUtils::vector_convert<CGALPoint>(v));
    }

    for (const auto &tv : mesh.triVerts) {
      B.begin_facet();
      for (const int j : {0, 1, 2}) {
        B.add_vertex_to_facet(tv[j]);
      }
      B.end_facet();
    }
    B.end_surface();
  }
};

template <class Polyhedron>
std::shared_ptr<Polyhedron> createPolyhedronFromManifold(const ManifoldGeometry& manifold)
{
  auto p = std::make_shared<Polyhedron>();
  try {
    manifold::Mesh mesh = manifold.getManifold().GetMesh();
    CGALPolyhedronBuilderFromManifold<Polyhedron> builder(mesh);
    p->delegate(builder);
  } catch (const CGAL::Assertion_exception& e) {
    LOG(message_group::Error, "CGAL error in CGALUtils::createPolyhedronFromPolySet: %1$s", e.what());
  }
  return p;
}

#endif


}  // namespace


/*!
   children cannot contain nullptr objects
 */
std::shared_ptr<const Geometry> applyMinkowskiManifold(const Geometry::Geometries& children)
{
  using Hull_kernel = CGAL::Epick;
  using Hull_Mesh = CGAL::Surface_mesh<CGAL::Point_3<Hull_kernel>>;
  using Hull_Points = std::vector<Hull_kernel::Point_3>;
  using Nef_kernel = CGAL_Kernel3;
  using Polyhedron = CGAL_Polyhedron;
  using Nef = CGAL_Nef_polyhedron3;

  auto polyhedronFromGeometry = [](const std::shared_ptr<const Geometry>& geom, bool *pIsConvexOut) -> std::shared_ptr<Polyhedron>
  {
    auto ps = std::dynamic_pointer_cast<const PolySet>(geom);
    if (ps) {
      auto poly = std::make_shared<Polyhedron>();
      CGALUtils::createPolyhedronFromPolySet(*ps, *poly);
      if (pIsConvexOut) *pIsConvexOut = ps->isConvex();
      return poly;
    } else {
      if (auto mani = std::dynamic_pointer_cast<const ManifoldGeometry>(geom)) {
        auto poly = createPolyhedronFromManifold<Polyhedron>(*mani);
        if (pIsConvexOut) *pIsConvexOut = CGALUtils::is_weakly_convex(*poly);
        return poly;
      } else throw 0;
    }
    throw 0;
  };
  
  assert(children.size() >= 2);
  auto it = children.begin();
  CGAL::Timer t_tot;
  t_tot.start();
  std::vector<std::shared_ptr<const Geometry>> operands = {it->second, std::shared_ptr<const Geometry>()};

  CGAL::Cartesian_converter<Nef_kernel, Hull_kernel> conv;
  auto getHullPoints = [&](const Polyhedron &poly) {
    std::vector<Hull_kernel::Point_3> out;
    out.reserve(poly.size_of_vertices());
    for (auto pi = poly.vertices_begin(); pi != poly.vertices_end(); ++pi) {
      out.push_back(conv(pi->point()));
    }
    return out;
  };

  try {
    // Note: we could parallelize more, e.g. compute all decompositions ahead of time instead of doing them 2 by 2,
    // but this could use substantially more memory.
    while (++it != children.end()) {
      operands[1] = it->second;

      std::vector<std::list<Hull_Points>> part_points(2);

      parallelizable_transform(operands.begin(), operands.begin() + 2, part_points.begin(), [&](const auto &operand) {
        std::list<Hull_Points> part_points;

        bool is_convex;
        auto poly = polyhedronFromGeometry(operand, &is_convex);
        if (!poly) throw 0;
        if (poly->empty()) {
          throw 0;
        }

        if (is_convex) {
          part_points.emplace_back(getHullPoints(*poly));
        } else {
          Nef decomposed_nef(*poly);
          CGAL::Timer t;
          t.start();
          CGAL::convex_decomposition_3(decomposed_nef);

          // the first volume is the outer volume, which ignored in the decomposition
          Nef::Volume_const_iterator ci = ++decomposed_nef.volumes_begin();
          for (; ci != decomposed_nef.volumes_end(); ++ci) {
            if (ci->mark()) {
              Polyhedron poly;
              decomposed_nef.convert_inner_shell_to_polyhedron(ci->shells_begin(), poly);
              part_points.emplace_back(getHullPoints(poly));
            }
          }

          PRINTDB("Minkowski: decomposed into %d convex parts", part_points.size());
          t.stop();
          PRINTDB("Minkowski: decomposition took %f s", t.time());
        }
        return part_points;
      });

      std::vector<Hull_kernel::Point_3> minkowski_points;
      
      auto combineParts = [&](const Hull_Points &points0, const Hull_Points &points1) -> std::shared_ptr<const ManifoldGeometry> {
        CGAL::Timer t;

        t.start();
        std::vector<Hull_kernel::Point_3> minkowski_points;

        minkowski_points.reserve(points0.size() * points1.size());
        for (const auto& p0 : points0) {
          for (const auto p1 : points1) {
            minkowski_points.push_back(p0 + (p1 - CGAL::ORIGIN));
          }
        }

        if (minkowski_points.size() <= 3) {
          t.stop();
          return std::make_shared<ManifoldGeometry>();
        }

        t.stop();
        PRINTDB("Minkowski: Point cloud creation (%d ⨉ %d -> %d) took %f ms", points0.size() % points1.size() % minkowski_points.size() % (t.time() * 1000));
        t.reset();

        t.start();

        Hull_Mesh mesh;
        CGAL::convex_hull_3(minkowski_points.begin(), minkowski_points.end(), mesh);

        std::vector<Hull_kernel::Point_3> strict_points;
        strict_points.reserve(minkowski_points.size());

        for (auto v : mesh.vertices()) {
          auto &p = mesh.point(v);

          auto h = mesh.halfedge(v);
          auto e = h;
          bool collinear = false;
          bool coplanar = true;

          do {
            auto &q = mesh.point(mesh.target(mesh.opposite(h)));
            if (coplanar && !CGAL::coplanar(p, q,
                                            mesh.point(mesh.target(mesh.next(h))),
                                            mesh.point(mesh.target(mesh.next(mesh.opposite(mesh.next(h))))))) {
              coplanar = false;
            }


            for (auto j = mesh.opposite(mesh.next(h));
                  j != h && !collinear && !coplanar;
                  j = mesh.opposite(mesh.next(j))) {

              auto& r = mesh.point(mesh.target(mesh.opposite(j)));
              if (CGAL::collinear(p, q, r)) {
                collinear = true;
              }
            }

            h = mesh.opposite(mesh.next(h));
          } while (h != e && !collinear);

          if (!collinear && !coplanar) strict_points.push_back(p);
        }

        mesh.clear();
        CGAL::convex_hull_3(strict_points.begin(), strict_points.end(), mesh);

        t.stop();
        PRINTDB("Minkowski: Computing convex hull took %f s", t.time());
        t.reset();

        CGALUtils::triangulateFaces(mesh);
        return ManifoldUtils::createManifoldFromSurfaceMesh(mesh);
      };

      std::vector<std::shared_ptr<const ManifoldGeometry>> result_parts(part_points[0].size() * part_points[1].size());
      parallelizable_cross_product_transform(
          part_points[0], part_points[1],
          result_parts.begin(),
          combineParts);

      if (it != std::next(children.begin())) operands[0].reset();

      CGAL::Timer t;
      t.start();
      PRINTDB("Minkowski: Computing union of %d parts", result_parts.size());
      Geometry::Geometries fake_children;
      for (const auto& part : result_parts) {
        fake_children.push_back(std::make_pair(std::shared_ptr<const AbstractNode>(),
                                                part));
      }
      auto N = ManifoldUtils::applyOperator(fake_children, OpenSCADOperator::UNION);
        
      // FIXME: This should really never throw.
      // Assert once we figured out what went wrong with issue #1069?
      if (!N) throw 0;
      t.stop();
      PRINTDB("Minkowski: Union done: %f s", t.time());
      t.reset();

      operands[0] = N;
    }

    t_tot.stop();
    PRINTDB("Minkowski: Total execution time %f s", t_tot.time());
    t_tot.reset();
    return operands[0];
  } catch (const std::exception& e) {
    LOG(message_group::Warning,
        "[manifold] Minkowski failed with error, falling back to Nef operation: %1$s\n", e.what());

    return applyPairwiseMinkowski(children);
  } catch (...) {
    LOG(message_group::Warning,
        "[manifold] Minkowski hard-crashed, falling back to Nef operation.");

    return applyPairwiseMinkowski(children);
  }
}

}  // namespace ManifoldUtils

#endif // ENABLE_MANIFOLD
