# CAD + FEM plan

mep already has a 3D modeler (`src/model3d_doc.*`, `Mode::Model3D`): a
polygonal one, in the Blender lineage — meshes, vertices, extrude/inset/
subdivide, materials, lights, keyframes. This plan adds a *second*,
architecturally separate modeler in the CAD lineage — exact geometry, a
parametric feature history, a constraint-driven sketcher — and ties it to
an in-house finite-element stack whose boundary conditions live on CAD
topology rather than on mesh nodes.

The two modelers share almost nothing on purpose. `MeshData` is float32
parallel arrays with no notion of a face; a CAD kernel needs double
precision, exact surfaces, and named topology that survives a rebuild.
They meet at exactly one place: the tessellator, which turns a B-rep into
`MeshData` for display and export.

Status legend: `[ ]` not started, `[~]` partial, `[x]` done.

---

## Scope decisions (settled before writing this)

- **Full NURBS B-rep kernel.** Rational B-spline curves and surfaces,
  trimmed patches with p-curves, loft/sweep/offset, variable fillets,
  STEP fidelity. This is the OpenCASCADE-class path and the long one;
  Part C (surface–surface intersection) and Part E.3 (fillets) are where
  that cost actually lands.
- **All four physics families**, in this order: linear static structural
  → steady/transient heat → modal and linear buckling → nonlinear
  (large deformation, plasticity, contact).
- **API-first.** Every operation lands as a pure C++ function, then a Lua
  binding and an agent-RPC method, then UI on top of exactly that surface
  — the same order `model3d_doc.cpp` → `mep.model_*` → `model.*` →
  `mep_model_*` already follows, and the reason the 3D modeler is
  testable without a window.

## What the existing codebase constrains (verified, not assumed)

These are real findings from the current tree, and each one changes a
design decision below.

- **`gfx::Mesh::indices` is `unsigned short`** (`src/gfx/types.h:116`,
  uploaded as such in `backend_native_renderer3d.cpp:965`). That is a hard
  65,536-vertex ceiling per mesh. A FEM mesh of any interest exceeds it in
  the first minute. Fixed in Part 0.4, not worked around.
- **Per-vertex colors already work** (`mesh.colors` → `aVertColor`,
  `backend_native_renderer3d.cpp:62`, filled white when null). Scalar
  result fields (von Mises, temperature) can be coloured with no shader
  work at all. This is a genuine head start on Part J.
- **Undo in the 3D modeler is whole-`Scene` snapshots**
  (`std::vector<Scene>`, `model3d_doc.h`). A CAD document must not copy
  this: snapshot the *feature tree* (small, declarative) and recompute the
  evaluated geometry, which is derived data.
- **`JobManager` runs external processes, not in-process work**
  (`src/job.h`) — it forks/execs and only ever fires callbacks from
  `PollAll()` on the main thread, because the editor is deliberately
  single-threaded. That is the decisive argument for Part 0.5: the solver
  ships as a **separate `mep-fem` binary**, exactly as mep already ships
  `mep-org-lsp`, `mep-r-lsp`, `mep-cpp-lsp`. It keeps the frame loop
  untouched, gives a headless CLI solver for CI, and reuses Job's existing
  `Content-Length` raw-stdout framing (`rpc_framing.h`).
- **No numerics exist yet.** No Eigen, no BLAS, no sparse anything — grep
  confirms it. `vecmath.h` is 4×4 float graphics math. The entire linear
  algebra layer is greenfield, which is consistent with the project's
  posture (`docs/architecture.org`: "no UI toolkit, no browser engine, no
  PDF library") but means Part 0.3 is on the critical path for both the
  sketch constraint solver and every FEM solver.
- **Native-only at first.** `Editor::LoadFile` already refuses
  `.obj/.gltf` on wasm (`editor.cpp:28925`). CAD/FEM follows the same
  degradation, for the same reason plus threads and memory.

---

## Part 0 — Foundations

**Status: complete.** All six items land, with the deltas from what this
section originally specified recorded under each. Every claim below is
backed by a test in the repo; the numbers quoted are that test's own
output.

- [x] **0.1 `src/cad_math.{h,cpp}` — the double-precision core.** `Vec2d`,
  `Vec3d`, `Mat3d`, `Mat4d`, `Interval`, `Box3d`, the per-entity
  `Tolerance` triple, a small dense `MatrixNd` (LU, Cholesky, Householder
  QR least-squares), `NewtonBracketed`, `BrentRoot`, `BrentMinimize`,
  `NewtonSolve`, `LevenbergMarquardt`, and Gauss-Legendre plus adaptive
  Gauss-Kronrod quadrature. `mep-cad-math-test`, 991 checks.
  - Gauss-Legendre nodes are *computed* by Newton iteration on the
    Legendre recurrence rather than tabulated, and verified the only way
    that means anything: an order-`n` rule integrates every polynomial up
    to degree `2n-1` exactly, checked for every order to 16.
  - Worth knowing for Part A.5: on a flat (multiple) root, the residual
    and step convergence criteria are not interchangeable. `x^3` reaches
    `|f| <= 1e-12` while `|x|` is still `1e-4`, because the residual is
    the *cube* of the distance. Point inversion on a tangentially-touching
    surface meets exactly this, which is why `SolveOptions` carries both.

- [x] **0.2 Robust predicates.** `src/cad_predicates.{h,cpp}`:
  `Orient2D`, `Orient3D`, `InCircle`, `InSphere`, each a filtered fast
  path over an exact one, plus the `Expansion` arithmetic underneath
  (exposed, because Part C.4's boolean classifier needs exactly-signed
  determinants too). `mep-cad-predicates-test`, 107,266 checks in 47 ms.
  - **Delta:** Shewchuk interposes several progressively-more-precise
    stages between the filter and full exactness; this goes from the
    filter straight to exact expansion arithmetic. Same answers, a
    fraction of the code, slower only on inputs that reach the exact path.
    If Part G.3 finds that path hot — a structured mesh on a box generates
    cospherical point sets by the thousand — reintroducing the staging is
    a contained change behind an interface the tests already pin down.
  - Verified three ways: configurations degenerate *by construction*
    (integer points on a line, plane, radius-5 circle, radius-3 sphere);
    those same configurations perturbed by exactly one ulp, where the true
    sign is known and no inexact implementation can recover it; and the
    filtered path cross-checked against the exact path on 39,000
    randomised and near-degenerate inputs, which is what actually
    validates the error bounds.
  - **On `-ffp-contract=off`:** it is set for that translation unit, but
    the code was checked rather than assumed to need it. Built with
    `-mfma -ffp-contract=fast`, GCC emits 48 FMA instructions into the
    object and every test still passes bit for bit — `Split`'s product
    feeds two consumers so it cannot be fused, and `TwoProduct`'s
    subtractions remove exactly-representable products. The flag stays
    because that argument must be re-made after any edit to those six
    functions, and `PredicatesSelfTest()` is the runtime backstop.

- [x] **0.3 `src/num_sparse.{h,cpp}` — sparse linear algebra.** CSR with
  duplicate-summing triplet assembly, SpMV, transpose, `A^T A`,
  permutation, three fill-reducing orderings, sparse `LDL^T` with
  elimination tree and symbolic analysis, IC(0), preconditioned CG, and
  restarted GMRES. `mep-num-sparse-test`, 8,762 checks.
  - **Delta — simplicial, not supernodal.** The factorization is a
    textbook up-looking simplicial `LDL^T`. A supernodal one groups
    columns of identical pattern so the inner loops become BLAS-3 dense
    products; it will be several times faster on a large 3D problem. That
    is a bounded gap with a known fix (Part H.4), and taking it now buys a
    factorization small enough to read and test.
  - **Delta — minimum degree, not AMD.** The O(n·d²) formulation, not the
    near-linear approximate-minimum-degree algorithm with quotient graphs
    and supervariables. Above `kMinimumDegreeLimit` (20,000) it falls back
    to RCM, because past that the ordering costs more than the
    factorization it speeds up. Measured on a 20×20 grid Laplacian:
    natural 7,619 nonzeros in L, RCM 5,510, minimum degree 3,329.
  - **Deferred — sparse LU.** Unsymmetric *direct* solution is not here.
    Small dense unsymmetric systems go through `cad_math`'s `MatrixNd`,
    large ones through GMRES. Part I.6-I.8's tangent matrices are the
    first thing that will genuinely want it.
  - Verified by the method of manufactured solutions, not by a residual
    check: a Poisson problem with a closed-form solution, solved on two
    grids, shows an error ratio of **4.01** against the 4.00 that
    second-order accuracy predicts. A residual check proves only that the
    solver solved *the matrix it was handed*.

- [x] **0.4 32-bit index path in the renderer.** `gfx::Mesh::indices`
  widened to `unsigned int`; `GL_UNSIGNED_INT` in the element-buffer
  upload and in both `DrawElements` sites (the shadow pass as well as the
  main draw); the procedural `MeshBuilder` and the IQM importer updated.
  `-Wsign-conversion` caught the two places that had been silently
  widening `unsigned short` to `int` and now needed explicit casts.
  - This mattered more than the plan assumed. Every importer in the tree
    was *already* working around the 65,536-vertex cap by abandoning
    indexing and uploading flat triangle lists (see the OBJ and VOX
    importers' notes, and `BuildModel3DGpuMesh`). That costs three
    vertices per triangle and, decisively for Part J.2, destroys the
    vertex sharing a per-vertex field needs to interpolate smoothly.
  - New `mep-gfx-native-index32-smoke`: draws a 90,000-vertex indexed grid
    coloured by whether each index is above or below 65,536, and checks
    the framebuffer for the specific scrambling a wraparound produces.
    Confirmed to have teeth by temporarily truncating the indices, which
    fails it. Nothing else in the tree builds a large indexed mesh, so
    without this the widened type would be untested.

- [x] **0.5 `mep-fem` binary.** `src/fem_server.cpp`: JSON-RPC 2.0 over
  stdio with `Content-Length` framing via the existing `rpc_framing.h`,
  methods `fem/solve` (streaming `fem/progress` notifications),
  `fem/validate`, `shutdown`, `exit`, plus a `--solve <file>` CLI whose
  exit status reflects the solve so CI can branch on it without parsing
  output. Verified end to end through a real subprocess: six progress
  notifications, correct replies, an unrestrained model rejected with a
  usable message, unknown methods answered `-32601`, and the CLI and
  stdio paths agreeing to the digit.
  - The argument for a separate process got stronger on contact with the
    code: `src/job.h` is explicit that callbacks never fire off the main
    thread so editor state needs no synchronization. A solver thread would
    be the first thing in the codebase to break that, while holding the
    largest allocations in the process.

- [x] **0.6 Vertical-slice spike.** Box mesh → Hex8 linear elasticity →
  assemble → solve → von Mises → coloured deformed surface, drawn.
  `src/fem_model.*`, `src/fem_solve.*`, `src/fem_viz.*`; `mep-fem-test`
  (1,533 checks) and `mep-gfx-native-fem-smoke`.
  - **The element passes the patch test to machine precision.** An
    arbitrary linear displacement field imposed on every boundary node of
    an irregular 3×3×3 box is reproduced at the interior nodes with a
    worst error of **4.9e-19** against a field of scale 1e-4, and the
    stress is uniform to 1e-9 relative. That single test covers the shape
    functions, the Jacobian, the B matrix, the constitutive matrix, global
    assembly, constraint application and stress recovery at once.
  - Uniaxial tension under *consistent* nodal loads gives exactly the
    elementary answer — σ = 4.000000e6 Pa against an exact 4.0e6,
    elongation and Poisson contraction matching to 1e-9 — with a global
    equilibrium residual of 2.4e-15.
  - The cantilever converges monotonically from below toward Timoshenko:
    59.1% → 84.4% → 91.9% → 95.0% over four refinements. That is the
    documented shear-locking signature of a fully-integrated trilinear
    hex, not a defect, and B-bar/selective reduced integration (Part H.1)
    is the fix.
  - The rendered plot shows a dark neutral axis along mid-height where
    bending stress is zero. Nothing in the code puts it there; it falls
    out of the solve, which is the most convincing single piece of
    evidence the physics is right.

### Two real defects this part surfaced

Both were found by tests that were expected to pass, which is the point of
writing them.

1. **`SparseLDLT` accepted a near-singular matrix.** It rejected only an
   *exactly* zero pivot. A model free to rotate about a single fixed node
   has rigid-body pivots at round-off rather than at zero, so it
   factorized happily and returned garbage. Now rejected relative to the
   matrix's own largest diagonal (`singular_tolerance`, default 1e-13 —
   loose enough for a genuinely ill-conditioned model, tight enough to
   catch a rigid-body mode at 1e-16). This is the single most common user
   error in finite elements, so silently succeeding was the worst
   available behaviour.
2. **Constraint elimination wrecked the matrix scaling.** Constrained rows
   got a 1.0 on the diagonal while free rows carried ~1e11 — eleven orders
   of magnitude apart for no reason, and it made any relative singularity
   test impossible to threshold. Constrained rows now reuse their own
   original diagonal, with the right-hand side scaled to match; same
   solution, well-conditioned matrix, and defect 1 becomes detectable.

### What Part 0 deliberately does not do

Worth stating so nobody reads more into the vertical slice than is there.
There is no geometry kernel yet — the "box" in 0.6 is a mesh generator,
not a B-rep, so nothing is being *meshed* and no boundary condition is
bound to a face. Loads are nodal forces only: no pressure, gravity or
thermal load. Hex8 is the only element. Everything is linear. Those are
Parts A through I, in that order.

## Part A — Geometry: curves and surfaces

**Status: complete.** `src/cad_nurbs.*` (the B-spline engine),
`src/cad_curve.*`, `src/cad_surface.*`, `src/cad_mass.*`, with
`mep-cad-nurbs-test` (9,308 checks), `mep-cad-curve-test` (4,813),
`mep-cad-surface-test` (3,736) and `mep-cad-mass-test`.

- [x] **A.1 Interfaces and analytic families.** `Curve3` (Line, Circle,
  Ellipse, Nurbs) and `Surface` (Plane, Cylinder, Cone, Sphere, Torus,
  Nurbs, plus the derived kinds), each with evaluation, derivatives to
  any order, bounds, transform, closure queries and an exact NURBS form.
  The analytic families stay first-class, for the three reasons the
  headers spell out: closed-form intersections, lossless STEP round-trips,
  and being able to answer "what radius is this hole" without
  reverse-engineering control points.
  - Surfaces additionally carry the **first and second fundamental
    forms** and the curvatures derived from them. Not decoration: the
    first fundamental form is the metric Part G.2's mesher needs, and
    without it a face meshed by plain 2D Delaunay in (u,v) produces
    elements that are well-shaped in parameter space and arbitrarily
    distorted in 3D.

- [x] **A.2 NURBS curves.** de Boor evaluation, derivatives, knot
  insertion and removal, refinement, degree elevation, splitting,
  reversal, Bezier decomposition, interpolation and least-squares
  approximation. Every algorithm names the Piegl and Tiller algorithm it
  implements, and departures from the book are called out at the site.
  - Verified against an independent Cox-de Boor recurrence
    (`BasisFunctionDirect`, which shares no code with the fast path), by
    partition of unity, by finite differences for every derivative, and
    by *geometric invariance* for every shape-preserving operation.
  - **The exact circle is the sharpest check.** A circle is representable
    as a rational quadratic and by no polynomial, so evaluating the
    nine-point construction and measuring the radius tests the rational
    machinery against geometry with a closed-form answer: worst radius
    error **1.1e-16** over 401 samples. The curvature check
    (**6.7e-16** against an exact 1) goes further and pins the *second*
    derivative, which is where a missing binomial term in the
    quotient-rule correction would hide -- a first-derivative check
    passes with that bug present.

- [x] **A.3 NURBS surfaces.** Tensor-product evaluation and derivatives,
  knot insertion per direction, isocurve extraction, and the rational
  surface derivative correction. Verified against the full double sum over
  every basis-function pair, and by finite differences including the mixed
  partial -- the one term where both binomial sums interact.

- [x] **A.4 Derived surfaces.** Extrusion, revolution, ruled, offset, plus
  `LoftSurface` and `SweepSurface` as operations producing a
  `NurbsSurface` (which is how they are used; nobody wants a loft that
  re-runs skinning on every evaluation).
  - **The cross-validations are the point.** A cylinder built four ways --
    analytically, by extruding a circle, by revolving a line, and as a
    ruled surface between two circles -- agrees to **0.000e+00** in every
    case, through code paths that share almost nothing. A sphere built by
    revolving a meridian agrees with the analytic one to 1.2e-15.
  - Sweeps use **rotation-minimizing frames** (Wang's double reflection),
    not Frenet: a Frenet frame is undefined where the spine is
    momentarily straight and swings through 180 degrees at an inflection,
    which twists the profile visibly. Measured worst per-step twist over
    200 frames on an S-curve: 1.8e-4 radians, with orthonormality at
    3.3e-16.

- [x] **A.5 Inversion and projection.** Closest point on a curve and on a
  surface, two-stage (coarse sampling to locate the basin, then Newton --
  scalar for curves, a 2x2 solve for surfaces). Verified against brute
  force over dense sampling: **0.000e+00 excess** on 600 curve queries and
  160 surface queries. Also `ContainsPoint` and adaptive `Tessellate`,
  whose chord-tolerance promise is checked by sampling each output segment
  rather than only its midpoint.

- [x] **A.6 Mass properties.** Volume, area, centroid and inertia tensor
  by the divergence theorem -- a volume integral becomes a surface
  integral because the vector area element is exactly `(Su x Sv) du dv`,
  so no volume meshing is involved. Verified against closed forms for a
  box, sphere, cylinder and torus, all matching to 11-12 significant
  figures, with clean convergence under refinement (3.3e-5 to 1.4e-12).
  Reversing every face negates the volume exactly, which is the property
  Part C.5's boolean checks will rest on.
  - **Limitation, and it is why this is A.6 rather than Part B:** faces
    are untrimmed. Each contributes its whole rectangular parameter
    domain, so the surfaces handed in must already tile the boundary
    exactly. Once Part B gives faces real trimming loops the same
    integrals run over the trimmed region and nothing else changes.

### The parameterization caveat, which will matter in Part B

`ToNurbs` preserves *shape* exactly for every analytic family -- a NURBS
circle is at radius r to machine precision. It does **not** preserve the
parameterization inside a span. A rational quadratic traverses its arc
non-uniformly in its own parameter, so on a half circle the NURBS at the
parameter meaning "22.5 degrees" is actually at 21.6 degrees, while the
radius stays 1.000000000000000. The domain, both endpoints and every
internal span boundary do correspond exactly (verified at 4.4e-16).

This is inherent, not a defect: no NURBS can be parameterized by arc
angle, since that needs transcendental basis functions. It is recorded
here because **Part B.2's p-curves are where it will bite** -- an edge's
curve on a face must correspond to the 3D edge curve, and pairing
parameters across a conversion will silently be wrong. Invert with
`ClosestPoint` instead.

### Four real defects this part surfaced

1. **`InsertKnot` allocated one element too many** in both output arrays
   (`mp+r+2` where the book's indices run `0..mp+r`). The stray control
   point is never referenced, so the curve evaluated correctly everywhere
   and the bug was invisible to any point-comparison test -- the control
   point *count* caught it.
2. **`ClosestPoint` hung for ~1e283 iterations** on a query at the centre
   of a circle. Every point is equidistant there, making the Newton
   denominator `C''.(C-P) + |C'|^2` exactly `-r^2 + r^2 = 0`; rounding
   gave ~1e-17 instead, a step of ~1e284, and the domain-wrapping loop
   `while (next < lo) next += span` then ran essentially forever. Now
   guarded against non-finite and over-domain steps, and wrapped
   modularly rather than iteratively.
3. **`LoftSurface` discarded the weights**, projecting each control-point
   column to 3D before interpolating. Every rational section -- every arc
   and circle -- degraded into a polynomial through its own control
   points, wrong by 6% of the radius. Skinning now interpolates in
   homogeneous coordinates (`InterpolateHomogeneous`); the error went from
   6.066e-02 to 2.220e-16. The contrast that identified it: the ruled
   surface, which copies control points rather than interpolating, was
   exact all along.
4. **The offset degeneracy test had the sign backwards.** This file's
   second fundamental form is `II = <S_ij, n>`, which makes an outward-
   oriented sphere's curvatures *negative*; the textbook `1 + d*k = 0`
   condition assumes the opposite convention, so the correct test is
   `1 - d*k <= 0`. The convention is now stated explicitly in the header,
   because sign conventions in differential geometry are exactly the kind
   of thing that produces a silently folded surface.

## Part B — Topology

**Status: complete.** `src/cad_topology.*` (the B-rep and four
primitives), `src/cad_pcurve.*`, `src/cad_validate.*`,
`src/cad_tessellate.*`, `src/cad_naming.*`, with `mep-cad-topology-test`
(80,333 checks) and `mep-gfx-native-brep-smoke`.

- [x] **B.1 The B-rep data model.** Body → Shell → Face → Loop → CoEdge →
  Edge → Vertex, flat index-addressed arrays in one copyable `Model`, with
  geometry held by `shared_ptr<const>` so a copy shares it. Plus the four
  primitives, which are here rather than in Part E because a topology
  layer with no way to build a body cannot be tested at all.
  - The four are chosen to cover the cases that break implementations,
    not to be a useful library: the **box** has only ordinary faces, the
    **cylinder** has a seam, the **sphere** has a seam *and* two poles
    where the surface is degenerate, and the **torus** is genus 1 with two
    seams. All four validate, with counts and genus as predicted by hand
    (box V=8 E=12 F=6; cylinder 2/3/3; sphere 2/1/1; torus 1/2/1, genus 1).

- [x] **B.2 P-curves.** Each coedge carries its edge's curve in its own
  face's parameter space, represented as a `Curve3` confined to z = 0 —
  which means a straight p-curve is an exact `Line3`, a circular one an
  exact `Circle3`, and only the rest are fitted. A cylinder comes out as
  four lines and two circles, nothing fitted.
  - **Seams are placed by a geometric rule, not by chaining.** Walking the
    loop and putting each coedge next to its predecessor *cannot* work
    when a loop traverses the same edge twice: a sphere's face is bounded
    by one seam used twice, and chaining puts both uses on the same side,
    enclosing zero area. The rule that works is that a counter-clockwise
    outer loop keeps the face's interior to its left — so on a u-seam,
    travelling in +v belongs at `u_hi` and in −v at `u_lo`. It reproduces
    what the cylinder and torus already had and fixes the sphere.
  - Poles are handled by inheriting the parameter from a neighbouring
    sample, since projection at a degenerate point returns an arbitrary
    one.

- [x] **B.3 Validity checker.** Structural (references, one outer loop per
  face, loops closing as a vertex chain *and* geometrically),
  combinatorial (every edge used by exactly two coedges in opposite
  senses; Euler–Poincaré, which recovers genus rather than assuming it),
  and geometric (vertices on their edges, edges on both their faces,
  p-curves agreeing with the curves they represent, loops running the
  right way in parameter space). Diagnostics carry a stable category slug
  and the offending entity id.
  - **Eleven deliberate corruptions are tested, each asserting its own
    category.** A validity checker that never fires is indistinguishable
    from one that works, and the difference only appears much later, when
    an invalid body reaches the mesher. Missing face, flipped coedge,
    displaced vertex, two outer loops, scrambled loop order, three faces
    on one edge, a p-curve that wanders, a reversed loop, a missing
    p-curve (warning, not error), an open shell under a permissive option,
    and counts that violate Euler–Poincaré.

- [x] **B.5 Tessellator.** Edges tessellated once and shared, so the 3D
  points along a shared edge are one set of numbers used by both faces —
  which is what makes the result watertight. Faces triangulated by
  Delaunay over boundary *and* interior points, the interior grid sized
  from the surface's own curvature through the first fundamental form.
  - All four primitives tessellate **watertight** (every directed edge
    matched, welded by position) with **positive** volume converging on
    the analytic value: box exact at 12 triangles, cylinder and sphere to
    8e-4, torus to 1.6e-3.
  - `mep-gfx-native-brep-smoke` renders them, which catches what a volume
    check cannot: a misplaced seam leaves a visible scar, a badly handled
    pole pinches, and an inverted normal renders black.
  - **No super-triangle.** The Delaunay step starts from a single vertex
    at infinity rather than from a triangle big enough to hold the
    points, because no triangle is big enough: it is the circumcircles
    that have to be contained, and a sliver's circumradius is unbounded
    relative to the point set. The version that used one produced meshes
    with holes in them -- on an ordinary pipe, at the default tolerance.
    Found while verifying Part E.3 and written up there, because the
    diagnosis is more useful than the patch: the tell was that tightening
    the tolerance made the result *worse*.

- [x] **B.4 Persistent naming.** An entity is recorded by its generating
  feature and role, its geometry kind, a sample point and direction, and
  the features generating its neighbours; resolution is a weighted score,
  not a lookup. All six faces of a box resolve through a rebuild that
  changes every dimension.
  - **The design constraint is knowing when it does not know.**
    `Ambiguous` and `Lost` are first-class outcomes with no
    "best guess" convenience overload, and the weights are set so no
    single signal can resolve a name alone. Reporting ambiguity costs a
    user one click; resolving wrongly moves a hole to the other side of a
    part and says nothing.

### Five real defects this part surfaced

1. **The sphere's seam was at the wrong longitude.** It has to be at
   u = 0 specifically — the face is the whole parameter rectangle, so its
   boundary is at u = 0 and u = 2π, and a meridian anywhere else bounds
   nothing.
2. **Ear clipping is the wrong tool for a curved face.** It triangulates
   using only boundary vertices, which is right for a flat region and
   wrong for a curved one: a cylinder's tube is a rectangle in parameter
   space whose sides carry dozens of points collinear in (u,v), so a
   clipper either stalls on them or emits triangles whose three corners
   all lie on the bottom rim — flat triangles inside the end disc,
   nowhere near the surface. They cancel in pairs, which is why the
   tube's computed volume came out as **exactly zero**. Replaced with
   Delaunay over boundary and interior points, on the Part 0.2 predicates.
3. **Bridging every long parameter gap broke watertightness.** Adjacent
   faces subdivided the same edge independently and placed points at
   different parameters. Bridging now happens only where the two
   parameter points name the *same* 3D point, which is exactly what a
   pole is. The box's volume was exactly right throughout — only the
   closure test caught it.
4. **Dropping each coedge's last point** is right for an ordinary edge
   and wrong at a pole, where the next coedge starts at the same 3D point
   under a *different* parameter. The polygon never reached the pole, the
   bridge never fired, and the sphere collapsed to a sliver.
   Deduplication now happens in parameter space, after the fact.
5. **The interior-point test used a polygon it was appending to.** Each
   candidate was tested against a boundary that already contained the
   previously accepted candidates. The sphere's volume error fell from
   0.97 to 8e-4 when fixed.

### What Part B deliberately does not do

The Delaunay triangulation is **unconstrained**. Boundary edges are
respected for convex or nearly-convex faces — which is every face these
four primitives produce — and triangles straying outside are removed by a
centroid test. Genuinely non-convex faces arrive with Part C's booleans
and want constrained Delaunay with edge recovery, which is the same
machinery Part G.3 needs for volume meshing and is best built once, there.

Hole bridging likewise uses nearest-vertex rather than a full visibility
test, which is adequate until a boolean produces a deeply non-convex
outer boundary.

## Part C — Intersections and booleans

**Status: complete.** `src/cad_intersect.*`, `src/cad_classify.*`,
`src/cad_boolean.*`, with `mep-cad-intersect-test` and
`mep-cad-boolean-test`. Booleans work on planar and seam-periodic solids
(boxes, cylinders); a face whose parameterization has a pole is refused
with an explanation rather than cut wrongly -- see below.

- [x] **C.1 Curve-curve and curve-surface.** Every intersection of two
  curves, and every crossing of a curve through a surface, with tangential
  and coincident contacts reported as their own `ContactKind` rather than
  flattened into a point.
  - **Built on distance minimisation, not on a Newton solve**, and the
    difference is the whole robustness story. Newton on the stationarity
    conditions is faster and fails exactly where it matters: at a tangency
    the two tangents are parallel, its Jacobian is singular, and it
    reports *no hit at all*. A line tangent to a circle was missed
    entirely. Brent minimisation has no such degeneracy, and the
    projection it builds on is the one already verified exact against
    brute force in Part A.5.
  - Newton is kept as a **polish** after Brent: it is quadratic where it
    works and declines where it does not, so a transversal crossing
    reaches machine precision while a tangential one keeps Brent's answer.
    Robust global search plus fast local refinement, each doing what it is
    good at.

- [x] **C.2 The analytic table.** plane/plane, plane/sphere,
  plane/cylinder (perpendicular, parallel and oblique), plane/cone,
  plane/torus (both axis-aligned families), sphere/sphere,
  sphere/cylinder (coaxial) and cylinder/cylinder (parallel or coaxial).
  Unhandled pairs are **declined**, not answered wrongly.
  - Verified two ways. First the defining property, asserted for every
    branch of every case: each point of an intersection curve lies on
    *both* surfaces. Worst deviation over the whole table is 1.0e-7, which
    is the line-clipping tolerance itself; the curved branches sit at
    ~1e-15. Second, each case's closed form is checked directly -- a plane
    through a sphere's centre gives a great circle, an oblique plane cuts
    a cylinder in an ellipse whose minor semi-axis is the cylinder's own
    radius and whose major is that over `|n · axis|`, a plane through a
    torus's axis gives two circles of the tube's radius.
  - Coincident, tangential and disjoint are distinguished from
    transversal. That distinction is load-bearing: a boolean that treats a
    tangential touch as a crossing produces a zero-thickness sliver that
    then fails to mesh, and coincident faces must be merged rather than
    intersected.

- [x] **Classification (the middle third of C.4).** Point-in-solid by ray
  crossing parity, with `Boundary` as a first-class outcome. All four
  primitives classify inside, outside and boundary points correctly,
  including a torus, where the point on the axis is outside despite being
  inside the bounding box. Checked against the analytic answer on 119
  random points around a sphere, all agreeing.
  - The interesting part is the degeneracy handling. A ray that grazes a
    face, passes through an edge or hits a vertex is counted once, twice
    or not at all depending on arithmetic at the tolerance. Rather than
    trying to decide such a hit, the ray is detected as unlucky and a
    different one is fired, from a fixed deterministic spread -- because a
    classification that depends on a random number cannot be reproduced
    when it goes wrong.

### The defect worth recording

**The ray sampling and the grazing threshold have to be chosen together.**
A ray that nearly misses a body enters and leaves through a very short
chord; if that chord is shorter than one sampling interval the two
crossings collapse into one, parity comes out odd, and a point plainly
outside the body is reported inside. It happened once in 119 random
points. For a sphere of radius r a chord of length c meets the surface at
`|direction · normal| = (c/2)/r`, so a grazing threshold of g catches
every chord shorter than `2gr` and the sampling must resolve anything
longer. The two defaults are now set with an order of magnitude of margin
between them, and the relationship is written down where they are
declared rather than left to be rediscovered.

A second, smaller one: the polygon that a face's trimming loops are
sampled into must **include the p-curves' end points**. Stopping one
sample short leaves the polygon not reaching the corners of the parameter
region, and those corners are where the interesting points are -- a
sphere's poles sit exactly there, and a polygon short of them puts the
poles outside their own face.

### C.3, C.4 and C.5

- [x] **C.3 The general marcher.** Marching along the intersection with
  tangent stepping and Newton correction back onto both surfaces,
  constrained to the plane normal to the step so the correction cannot
  slide along the curve, then fitting the traced points to a NURBS.
  Verified against the analytic table on the cases both can do: a
  plane/sphere circle comes back 12.167335 long against 12.167336 exact,
  a sphere/sphere circle 8.311872 against 8.311873, and two crossing
  cylinders give two closed branches whose worst distance from either
  surface is 1.02e-07.
  - **Three things it had to learn, each from a wrong answer.** Points
    clustered at the minimum step make the fit ill-conditioned -- a curve
    through points every one of which was on both surfaces strayed 0.13
    away from them -- so the trace is thinned before fitting and the fit
    is verified afterwards, with a branch that deviates too far dropped
    rather than returned. A seam is not a domain exit: treating it as one
    split a sphere/sphere circle into two half-circles and two crossing
    cylinders into four pieces. And a closed loop must be closed by
    pushing the seed on as the final point, not by trusting the last step
    to land on it, which left every loop about half a percent short.
  - A tangential contact is reported as `Tangent` with a note, not as
    `None`. The marcher cannot step along a curve whose direction is the
    cross product of two parallel normals, and saying so is the honest
    answer.

- [x] **C.4 Imprint and stitch.** `BooleanOperation` for union,
  intersection and difference: intersect every face pair, trim each
  branch to the part lying on both faces, cut each face by building a
  planar arrangement of its trimming loops and the new curves in its own
  parameter space, classify each region of that arrangement against the
  other solid, and stitch the surviving pieces into a shell.
  - **Verified by identity, not by eye.** `V(A∪B) + V(A∩B) = V(A) + V(B)`
    holds *exactly* under tessellation, because both sides facet the same
    surfaces the same way and the faceting error cancels. A cylinder
    drilled through a box gives 6.465518 + 1.534482 = 8.000000000 against
    the box's own volume, with each part within the faceting error of the
    closed form. Every result also goes through Part B.3's validator and
    is checked watertight after tessellation -- an open shell still has a
    volume, so the volume alone would not have caught it.
  - **Cases that are answered rather than cut.** When no face pair
    intersects, the answer is one of the inputs: the solids are disjoint,
    or one lies wholly inside the other. `A - B` with `B` inside `A` is
    `A` carrying `B` reversed as an inner shell bounding a cavity, which
    validates as a two-shell body and tessellates to 64 - 8 = 56.
  - **What it refuses, and why that is the right answer.** A face whose
    parameterization has a pole -- a sphere's poles, a cone's apex, a
    revolution meeting its axis -- cannot be cut: the arrangement works in
    parameter space, and at a pole the face's boundary there is a
    degenerate line carrying no p-curve, so the parameter rectangle never
    closes and the cycles that would bound the pieces do not exist. An
    intersection curve crossing the seam also arrives unwrapped, running
    outside the face's own domain. Both are fixable and neither is small,
    so the case is detected up front and refused with a message that says
    what is wrong. Coincident faces are refused for the reason already
    given in C.1.

- [x] **C.5 Boolean fuzzing.** Forty random box pairs per run, each
  constructed to be a genuine corner overlap rather than hoped to be one
  -- a first attempt generating both boxes freely refused 58 of 60 as
  disjoint or nested, which tests the shortcuts instead of the boolean.
  Each triple is checked against both identities, against the closed form
  for the overlap, and for the predictable piece counts a corner overlap
  must produce (12 faces for the union, 6 for the intersection, 9 for the
  difference). Worst identity error across a run is about 8e-07 on
  volumes of order 100.
  - The OpenCASCADE cross-check named in the original plan is not done.
    The identities and the validator turned out to be the sharper
    instrument: every real defect found here -- three dropped face
    pieces, a slit that failed to divide a face, a sliver a few
    nanometres long -- broke one of them immediately, and none of them
    would have been visible in a volume compared to four digits.

### Bugs this part cost, worth keeping

- **A face that comes out uncut reports nothing.** A branch clipped too
  short leaves a slit the arrangement walks *around* instead of a cut
  that divides the face, and the result is a region of the full, uncut
  area -- no error, no warning, just a piece count one lower than it
  should be. Two separate causes produced it: a plane/plane line sampled
  in a window centred on the line's own origin rather than on the
  surfaces, and a run shorter than one sample interval never being seen
  at all.
- **A closed curve inside a face has one endpoint, so it cannot become a
  half-edge.** A drilled hole is exactly that case, and without cutting
  such a segment in half the plate came out solid.
- **A closed curve also gives two cycles along the same edges**: the
  region it encloses and the hole it leaves in the region around it. A
  containment test alone hands the hole to its own twin, so the
  surrounding region never learns it has a hole, and the enclosed region
  is declared a hole in itself and dropped for having no interior point.
- **An interior point must be deep, not merely inside.** Grid lines land
  on a region's own edges, where the parity test answers by round-off,
  and such a point sits on the *other* solid's boundary too -- so it
  classifies as neither inside nor outside and the piece is silently
  dropped. The deepest grid point is taken instead, refined until it is
  inside by a margin worth believing, and several are kept so that
  classification can ask again somewhere else.

### Two bugs in earlier parts that only booleans could expose

- **The tessellator sampled each edge in ascending curve parameter**,
  while `BuildLoopPCurves` walks the edge from `t_start` to `t_end`. Every
  edge a box makes runs the same way, so the two never disagreed until a
  boolean produced edges that run backwards along their curve -- and then
  the boundary polygon came out scrambled.
- **The tessellator's test for a degenerate point measured the angle
  between the two surface derivatives, not the area element.** At a
  sphere's pole they stay exactly perpendicular and it is `dS/du` alone
  that vanishes, so the pole was never recognised. It went unnoticed
  because `ClosestPoint` happened to return a sensible `u` for a point a
  fraction off the axis; snapping edge endpoints onto their vertices --
  needed so that three faces meeting at a corner actually meet in space
  -- put the point exactly on the axis, where `ClosestPoint` returns an
  arbitrary longitude, and the sphere stopped being watertight.

## Part D — Sketcher and constraint solver

**Status: complete.** `src/cad_sketch.{h,cpp}`,
`src/cad_constraint.{h,cpp}`, `Mode::CadSketch` in `editor.cpp` and
`main.cpp`, the `mep.sketch_*` Lua functions and the `sketch.*` agent-RPC
methods, with `mep-cad-sketch-test` (windowless) and
`mep-cad-sketch-live-test` (against a real running mep). The planar arrangement the profile finder needs
was lifted out of the boolean into `src/cad_arrangement.{h,cpp}`, so
there is one implementation of it rather than two.

- [x] **D.1 2D sketch geometry.** Points, lines, arcs, circles, ellipses
  and B-splines on a sketch plane, which may stand alone or be attached
  to a model face through Part B.4's persistent name -- a sketch on a
  block's top face is still on the top face after the block gets taller,
  verified against a rebuild that changes every dimension. Construction
  geometry is flagged and excluded from profile extraction.
  - **Points are the degrees of freedom and entities share them.** An arc
    is a centre and two endpoints rather than a centre, a radius and two
    angles. The second form is the obvious one and it is wrong for a
    sketcher: the commonest thing a user does is join one curve's end to
    another's, and in that form the join is a constraint between a point
    and a *derived* quantity. In this one it is two points being the same
    point. The price is a redundant representation -- six numbers for an
    arc's five degrees of freedom -- and that missing equation is added
    explicitly rather than maintained on the side, so the
    degree-of-freedom count stays honest.
  - A circle keeps a radius scalar rather than a point on the rim,
    because a circle has no distinguished point and inventing one would
    give the solver a rotation to find that does not exist.

- [x] **D.2 The constraint solver.** All sixteen kinds the plan names,
  plus the internal one an arc carries, solved as a nonlinear
  least-squares problem by Levenberg-Marquardt over Part 0's dense
  factorisation, decomposed first into independent clusters.
  - **The Jacobian comes out of the residual, not out of a second
    derivation.** Each constraint is written once in terms of a dual
    number carrying its own partials, so the derivatives are the chain
    rule applied by the arithmetic. They are exact, not finite
    differences, and they cannot drift out of step with the residual
    because there is only one piece of code. The test still checks them
    against central differences -- which now checks something real: that
    the residual means what the constraint says and that each partial
    lands in the right column. Worst disagreement across all twenty-five
    cases is 3.0e-10.
  - **Residuals are scaled to be lengths or angles.** A parallelism
    written as a bare cross product scales as a length squared, and on a
    sketch a hundred units across it would outweigh a coincidence by a
    factor of a hundred -- the sketch would come out parallel and not
    quite joined up. Every residual is divided back down.
  - Verified against closed forms: a 3-4-5 triangle comes out with the
    cosine of its right angle at -7.4e-17, a line tangent to a circle at
    exactly the radius from its centre, two externally tangent circles
    with their centres exactly the sum of the radii apart.

- [x] **D.3 Diagnosis and dragging.** Degree-of-freedom counting from the
  Jacobian's numerical rank, the null-space basis reported as drag
  handles, and redundant-versus-conflicting discrimination.
  - **The discrimination is the point, and it is not a heuristic.** Both
    look like rank deficiency. What tells them apart is the *left* null
    space: combinations of constraint rows the parameters cannot
    distinguish. Each such combination is either consistent with the
    residual, in which case the constraints agree and the extra one says
    nothing new, or it is not, in which case nothing satisfies them. A
    rectangle with a correct diagonal dimension and the same rectangle
    with a wrong one differ in exactly that and in nothing else.
  - **Dragging moves along the free directions, it does not pin the
    point.** Pinning is the obvious implementation: put the point where
    the pointer is and solve. It refuses the whole drag whenever the
    pointer goes somewhere unreachable -- dragging a dimensioned
    rectangle's corner sideways refuses the vertical part of the motion
    too, which was perfectly possible. The requested motion is fitted
    onto the null-space basis in the least-squares sense and the solve
    then cleans up the second-order drift.
    - Fitting rather than projecting matters. A rectangle's height
      direction moves two corners together, so each unit of it moves the
      dragged corner by only 1/sqrt(2); projecting delivers half the
      requested motion per step and sixteen steps still leave the corner
      visibly short.

- [x] **D.4 Profile extraction.** The closed regions the non-construction
  geometry divides the plane into, with nested regions reported as holes
  of the region containing them rather than as separate profiles -- a
  washer drawn as two circles is one profile with a hole, and an extrude
  built on the other reading would fill it in.
  - Areas are integrated along the curves by Green's theorem, not
    measured off the polygon the arrangement samples them with. Eight
    points per edge is ample for deciding topology and gives a circle's
    area 2.5% small, which is the sort of number a user reads off a
    dialog and does not forget.

- [x] **D.5 `Mode::CadSketch`.** The sketch pane: digits pick a drawing
  tool, letters apply a constraint to the selection by first letter, and
  a dimension opens `Mode::Prompt` pre-filled with what the selection
  currently measures. Geometry is drawn from `Sketch::Curve` -- the same
  call profile extraction uses, so the picture cannot disagree with the
  geometry -- with construction geometry dashed, closed profiles labelled
  with their exact area, and the solver's verdict always on screen rather
  than only when asked.
  - **It has no logic of its own, and that is checked rather than
    intended.** Every key and every click ends in one of the
    `Editor::CadSketch*` methods, which are the same ones the
    `mep.sketch_*` Lua functions and the `sketch.*` agent-RPC methods
    call. `mep-cad-sketch-live-test` drives both surfaces against a real
    running mep and compares: the rectangle tool driven by real clicks
    produces the same four constrained lines as `sketch.addRectangle`,
    and `e` on two selected edges produces the same constraint as
    `sketch.constrain`.
  - The sidebar listing constraints is not built; the status band carries
    the DOF state and the diagnosis instead, which is the part of it that
    matters while drawing.

### Three things the interactive tool got wrong first

- **Dragging a point cannot pin it.** Pinning the point where the pointer
  is and solving refuses the whole drag the moment the pointer goes
  somewhere unreachable -- dragging a dimensioned rectangle's corner
  sideways refused the vertical part of the motion too. Fitting the
  requested motion onto the null-space basis instead goes as far as the
  sketch allows, which is what a user means by dragging.
- **Auto-fitting the view when geometry appears yanks it out from under
  the user.** It is wanted, but only for a sketch that already existed
  the first time the pane was drawn -- an agent's sketch would otherwise
  be half off the edge. The flag is set on the first frame whether or not
  there was anything to fit, so an interactive session never re-fits.
- **A new sketch buffer is pristine by every test the editor applies**,
  so the startup dashboard drew straight over it. The same is true of a
  new 3D scene and a new procedural image, and `BufferIsPristine` already
  said so in a comment; the sketcher simply had to be added to the list.

### A bug in Part 0 that only a sketch could expose

**Levenberg-Marquardt scaled its damping by each parameter's own
curvature and nothing else.** That is Marquardt's own prescription and it
is right where every direction is curved. A sketch routinely has
directions that are nearly flat -- a construction line's length, which no
constraint depends on except through a second-order term -- and there the
Gauss-Newton step is a gradient divided by almost nothing. A ten-unit
centreline was stretched to fourteen hundred units, and the residual was
barely worse for it, which is exactly why nothing pushed back. Flooring
the damping at a fraction of the largest curvature in the problem keeps
those directions where they were, which for an under-constrained system
-- the normal state of a sketch -- is the only sensible answer.

### Added to Part 0 for D.3

**Singular value decomposition** (`Svd`, `MatrixRank`, `NullSpace` in
`cad_math.h`), by one-sided Jacobi. A constraint Jacobian has to be asked
what its numerical rank is and what directions it leaves free, and no
factorisation-with-pivoting answers either honestly: a nearly dependent
constraint is a small singular value, not a zero pivot. One-sided Jacobi
rather than Golub-Kahan because it computes the small singular values to
high relative accuracy -- which is exactly what separates "redundant"
from "very nearly redundant" -- and because it is sixty lines of
rotations that can be read, against several hundred for bidiagonalisation
and an implicit QR sweep.

## Part E — Feature modeling

**Status: Part E complete.**
`src/cad_feature.{h,cpp}`, with `mep-cad-feature-test`.

- [x] **E.1 The feature tree and rebuild.** An ordered list of parametric
  features, each naming its sketch and its numbers, replayed from the
  first dirty one. Changing a dimension and rebuilding produces the part
  the new dimension describes -- a plate thickened from 2 to 5 keeps the
  hole that goes through it going through it -- which is the only thing a
  feature tree is for.
  - **A failure does not stop the rebuild.** It is recorded on the
    feature that caused it, named, and the features after it are still
    attempted. A tree that stops at the first broken feature tells you
    about one problem at a time, and a part with three broken features
    then takes three rebuilds to understand.
  - Suppression takes a feature out without removing it; `NewBody` leaves
    the previous body alone and adds a second, which is what a multi-body
    part is; a `Cut` with nothing to cut is refused rather than quietly
    becoming a `NewBody`, because that is almost never what was meant.

- [x] **E.2 Sketch-based features.** Extrude (blind, symmetric and
  through-all, either way along the sketch normal), revolve (any angle
  including a full turn, about a construction line so the axis stays
  parametric), loft between corresponding sections, and sweep along a
  spine with twist and scaling -- each with its Add, Cut and Intersect
  variants through Part C.4.
  - **A sweep's frame is rotation-minimizing, not Frenet.** The Frenet
    frame is undefined wherever the spine is momentarily straight and
    spins through 180 degrees at an inflection, so a Frenet sweep along an
    S-curve twists the profile visibly and one along a straight run has no
    frame at all. Part A.7's double-reflection frame is defined everywhere
    the tangent is and carries no twist of its own, leaving whatever twist
    the feature asks for and no more. Checked against Pappus: a square
    section swept round a quarter circle comes out 31.3915 against the
    31.4159 its centroid's travel says.
  - Loft and sweep share one construction, because they *are* one: a
    stack of sections, corresponding pieces joined by a ruled surface,
    and a flat cap at each end. Only where the sections come from
    differs.
  - **The profile-to-solid construction is the substance of it**, and the
    orientation conventions are the whole difficulty. A solid built with
    one of them backwards is perfectly valid, perfectly watertight, and
    has a *negative* volume -- so the sign of the volume is itself a test,
    and it is what caught the revolve.
  - **A revolve's sweep direction cannot be read off the angle's sign.**
    It is `axis x radius`, so a profile to the left of the axis sets off
    one way and its mirror image the other, for the same axis and the
    same angle. Assuming otherwise builds the part inside out.
  - **A loft between sections that do not correspond is refused.**
    Matching a triangle to a circle needs the sections re-parameterised
    against each other first; guessing a correspondence produces a
    twisted, self-intersecting result that still validates.
  - **A cutting tool is pushed a hair back past its own start**, because
    the commonest operation in CAD -- a pocket sketched on the face it
    starts from -- puts the tool's end cap exactly on that face, which
    C.4 refuses. The distance is lengthened to match, so the floor stays
    where it was asked for. What it costs is stated in the code: a cut
    sketched on a plane with material above it reaches that far into it.
  - **Through-all is measured, not guessed.** A tool four times the
    part's diagonal makes every intersection it takes part in worse
    conditioned, and the endpoints of a hole's rim came back three
    microns apart -- far enough for the stitch to see two vertices and
    leave the shell open.

### Canonical surfaces, and why they are not an optimisation

A line swept is a plane and a circle swept along its own axis is a
cylinder. Building those as generic `ExtrusionSurface`s instead is
correct geometry and wrong engineering: Part C has closed forms for
plane/plane and plane/cylinder and a marching solver for everything else,
and handing it an extrusion wrapping a circle means marching around a
surface that closes on itself. A hole drilled through a plate came out
non-manifold for exactly that reason, while the identical hole built from
`MakeCylinder` came out clean.

Recognising them brings its own conditions, each of which produced a
wrong answer before it was understood:

- **The cylinder's axis must be the sweep direction**, or the surface
  extends away from the solid it bounds and the boolean finds nothing to
  intersect at all.
- **Its angle then runs backwards** whenever the sweep opposes the
  circle's own normal, and the face's loop has to follow that. Getting it
  wrong does not produce a wrong solid: it produces a face whose p-curves
  land a full turn outside its surface's domain, where nothing can find
  them. The face vanishes, the body still validates, and a boolean
  against it silently finds nothing to cut.
- **A closed profile loop is one face with a seam, not two half-faces.**
  The arrangement splits every closed curve in half on purpose -- one
  with nothing crossing it has a single endpoint and cannot become a
  half-edge -- and for deciding topology that is right. For building a
  solid it doubles the seams, the surfaces and the edges the boolean must
  match up. Putting it back together is what makes an extruded circle
  into the cylinder the rest of the kernel already knows how to cut.

### Three gaps in Part C.4 that only Part E could expose

All three were invisible to the boolean's own tests, because `MakeBox`
and `MakeCylinder` happen to avoid them and nothing else was cutting
anything.

- **An intersection curve that runs all the way round a periodic face is
  described by a p-curve running outside that face's parameter
  rectangle.** Unwrapping for continuity is right -- a p-curve that
  jumped by a full turn in the middle would be useless -- but it means
  the arrangement cannot see the curve meeting the face's own seam edges,
  the face comes out uncut, and the boolean returns the solid unchanged
  with no error. `MakeCylinder` never showed it because the analytic
  plane/cylinder intersection happens to build its circle on the
  cylinder's own frame. A tube built by extruding a sketched circle has
  no reason to line up that way.
- **The piece either side of the seam then has to be put back in the
  right copy of the rectangle.** At the seam, which side a point belongs
  to is genuinely ambiguous, and it came back next door about half the
  time -- perfectly shaped and in the wrong place, so the arrangement
  dropped it for having nothing to connect to.
- **Inverting a point on a closed curve is ambiguous at its seam**: the
  start of the circle and its end are the same place. Taking a piece's
  two ends independently produced, about half the time, a range running
  the long way round -- a quarter of a rim came back claiming to be the
  other three quarters, so two faces of one hole claimed the same edge
  and a third went unused. Walking the piece through its middle settles
  it.

### E.6: datums, and a sketch solver with three more dimensions

`src/cad_assembly.{h,cpp}`. A datum is a piece of geometry with no
material -- a point, an axis or a plane. Reducing every reference to one
of those three is what keeps the mate solver from needing to know
anything about B-reps: a planar face, a cylindrical face, a straight
edge, a hole's rim, the line where two planes meet and the point where an
axis pierces a plane all arrive at the solver as the same three kinds.

An assembly is instances and mates, and solving it is a least-squares
problem in exactly the way a sketch is. It is solved by the same
machinery, which is why `cad_dual.h` now exists: Part D.2's forward-mode
dual numbers were lifted out of `cad_constraint.cpp` and shared rather
than copied, because the softening in them is the kind of thing that gets
fixed in one place and forgotten in the other.

**What is different in three dimensions is the rotation**, and how it is
written down decides whether any of this works. Euler angles lock. A
rotation matrix is nine numbers with six constraints between them. This
uses a quaternion: four numbers, one constraint, no singularity anywhere,
and rotation by it is a *polynomial* -- so the dual arithmetic
differentiates it exactly, with no special cases and no finite
differences.

Two details of that are worth stating because each is load-bearing:

- **The rotation divides through by the quaternion's squared norm**, so
  it is a rotation whether or not the quaternion currently has unit
  length. That is what lets the solver take a step that stretches it
  without the geometry lurching; the normalisation is then pulled back by
  an ordinary residual rather than having to hold exactly true at every
  trial point.
- **That normalisation residual is also what makes the degree-of-freedom
  count come out right.** Seven parameters an instance, one of them spent
  on the gauge, leaves six. Without it the solver has a direction it can
  move in that changes nothing, and every instance reads one degree of
  freedom too free.

**Redundant is not the same as inconsistent.** The same mate twice is
harmless -- the count reads the Jacobian's *rank*, so it is unchanged and
nothing is reported. Two mates that cannot both hold are a different
thing: least squares splits the difference and the report names both,
because which one to give up is not the solver's decision.

Several mates are deliberately over-determined -- three numbers for the
two-parameter condition "these directions are parallel" -- because the
redundant form is the one without a singularity in it. Concentric is a
cross product rather than a difference, so a pin fits its hole either way
round, which is what people mean by concentric and is not what "the
directions are equal" would give.

#### Verifying it

**The degree-of-freedom count is the sharpest test here**, sharper than
any position, because it reads the rank of the Jacobian rather than the
answer: it says what the mates *mean*, not where the solver stopped. Two
plates, one on the other, tell the whole story in four numbers:

    no mates                6 degrees of freedom
    + faces flush           3
    + one hole lined up     1
    + a second hole         0

The middle step is the one that catches a badly formulated mate: a
concentric mate removes four, but two of those go to angles the flush
mate had already taken, so it is worth two there and not four. A mate
that removed the wrong count gets caught by that number even when it
happens to produce a plausible placement.

The placements are then checked exactly, because every mate used has a
unique solution by construction -- so "converged" and "converged to the
right answer" are different claims and both are made. The two-plate case
starts from a placement nowhere near the answer, rotated on a tilted axis,
and reaches the exact one in six iterations with a residual of 4e-16. A
pin started upside down still lands on its axis, which is the test that
concentric really is a cross product. Angles are held to 1e-7 at 30, 45,
90 and 120 degrees.

Also checked: that an assembly with nothing grounded keeps the six
degrees of freedom of picking the whole thing up, which no amount of
mating removes; that a solved assembly flattens into real bodies with the
right volumes in the right places, which is the check that a placement is
a transform and not just a number in a report; and a three-part assembly
-- base, bracket, pin -- solved to zero degrees of freedom for the
bracket and one for the pin, being its own spin about its axis.

#### Sub-assemblies, and mates that survive a rebuild

**A sub-assembly is rigid from outside.** Its own mates are solved within
it and the parent places it as one piece, which is what makes a large
assembly tractable: the outer solve carries six parameters per
sub-assembly rather than six per part inside it. `SolveDeep` works
innermost first, because a sub-assembly's shape has to be settled before
there is anything to place. Flattening unpacks the nesting all the way
down, composing the transforms one level at a time -- tested three levels
deep, where a transform applied twice instead of composed would put eight
cubes somewhere else entirely.

**A mate has to survive the part being rebuilt, not just moved.** A mate
that remembers "the plane z = 1" is wrong the moment the plate is
thickened; one that remembers "the face this names" is still right, and
Part B.4 is what turns the name back into a face. `CaptureMateEnd`
records the name, `RebuildParts` replays every feature-tree-backed part
and re-resolves, and a part can be held as a `FeatureTree` rather than a
finished body -- so changing a dimension and rebuilding moves the whole
assembly to match.

Tested both ways round, because only the pair is convincing: a plate
thickened from 1 to 4 to 2.5 carries the plate above it with it, exactly;
and a mate written against a coordinate instead of a face does *not*
follow, which the test makes visible by putting the two in contradiction
-- the only way a solver can tell you one of them is stale.

**Lost is not the same as unsatisfied**, and they are reported
separately, because they are different problems with different fixes: one
means the assembly is contradictory, the other that a mate points at
something no longer there.

An aside worth recording, since it took a wrong turn to find: an empty
role in `CaptureMateEnd` means "whatever this entity calls itself", and
that is almost always what is wanted. A role invented at the call site
scores against nothing in the rebuilt model and turns a resolvable
reference into a lost one. Conversely, mislabelling a name is *not*
enough to lose it -- a name with the wrong role and the wrong surface
kind still resolves, because the sample point, direction and
neighbourhood all still agree. That is B.4's scoring working as designed,
and the test that a reference can genuinely be lost had to be built from
a reference to something that really is not there.

### E.5: a pattern is a list of placements

`TransformBody` and `PatternBody` in `src/cad_pattern.{h,cpp}`, with five
generators beside them.

Linear, grid, circular, mirror and sketch-driven are not five operations.
They are five ways of writing down a list of rigid transforms, after
which one routine copies the body once per entry. So the generators are
pure functions returning `std::vector<Mat4d>` and never touch a body,
and `PatternBody` is the only thing that does. Table-driven patterns are
then free rather than a sixth implementation: a caller with its own list
hands it straight over.

Each generator's first entry is the identity, so the seed is placement
zero rather than a special case every caller has to remember.

**The one ambiguity a circular pattern always has** is whether `count`
copies over an angle means a copy at each end or a step that leaves the
last one open. Six copies over a full turn want 60 degrees apart; six
over 90 degrees usually want 18 and a copy at each end. There is no
default that is not wrong half the time, so it is a parameter.

**Merging is where the boolean's limits show through.** Copies that
overlap transversally merge; copies that merely abut share a whole face,
which is the coincident-face case Part C.4 does not handle, and the
refusal names which copy could not be merged. The separate form always
works, and a multi-body part is a perfectly good answer.

#### Mirroring is not a rigid motion

A reflection reverses handedness, and a body mirrored without noticing
is perfectly valid, perfectly watertight, and has a negative volume.
That much was expected. What was not is that the correction splits by
surface type, and that the two halves are opposite:

- **A plane** is stored as two axes and both of them reflect, so its
  `(u, v)` survives untouched. Its surface normal flips, so the face's
  orientation flag must flip; its loop is left alone.
- **Anything parameterized by an angle** -- cylinder, sphere, cone, torus
  -- is stored as an axis and *one* reference direction, with the second
  recovered as a cross product. A cross product comes out the other way
  round under a reflection, so the angle runs backwards. Its surface
  normal therefore comes out already correct, so the flag must *stay*;
  but its parameter space has been mirrored, so the loop must be turned
  round to stay counter-clockwise.

Applying either correction in both cases gives a body that validates and
is inside out. `TransformBody` asks the question per surface rather than
per body, by sampling: if the transformed surface carries the same
parameters over the same points, it is the first case, and if it carries
none of them, the second. A surface that did both over different parts
of itself is refused rather than guessed at.

#### A Part A defect this turned up

`Transform` on all four angle-parameterized surfaces left the trimmed
domain alone while reversing the parameterization, so a reflected face's
boundary landed outside the surface it belonged to. The surface was right
as a *set* and wrong as a parameterization, which nothing notices until
something trimmed asks where its boundary went: the first symptom was a
mirrored fillet whose cylindrical face reported a loop enclosing no area
at all.

Fixed in `cad_surface.cpp` by negating the `u` domain end for end when
`Mat4d::LinearDeterminant()` is negative, with the regression test in
`mep-cad-surface-test`: for all four surfaces the reflected surface at
`-u` equals the reflection of the original at `u`, to 2e-15, and a plane
is checked alongside them for keeping its parameters exactly. Nothing
produced a reflection before this part, which is why it had gone
unnoticed -- worth recording, because it is the second time in Part E
that a piece of Part A turned out to be untested in the direction Part E
needed it.

#### Verifying it

For separate copies the volume is exactly `n` times the seed's, which is
worth checking because both ways of getting it wrong survive everything
else: a copy placed on top of another still counts once in a face tally
and twice here, and a mirrored copy that kept its handedness is valid,
watertight and negative. So every body is validated, tessellated,
checked watertight and checked *positive* before its size is looked at.

Merged patterns are checked against inclusion-exclusion, with the copies
stepped diagonally so that neighbours meet in an ordinary box of overlap
and no faces are shared -- stepping along one axis leaves four faces
coplanar with the neighbour's, which is the case the boolean refuses. The
tolerance there is 1e-6 rather than 1e-9, and for a reason worth naming:
a merged body's edges were *computed*, by intersecting surfaces to a
tolerance, while a separate pattern's were carried across unchanged.

Also checked: that a rigid motion leaves the volume alone; that
reflecting twice is the identity; that the direction vector's length does
not change a linear pattern's step; that moving a sketch point moves its
copy, which is the only reason to drive a pattern from a sketch; that a
full turn asked for with a copy at each end drops the repeat rather than
stacking two bodies the boolean cannot separate; and that a block
filleted by E.3 patterns and mirrors correctly, since a curved face
reflected without turning round is exactly how a mirror bug hides.

### E.4: four operations, one routine

`OffsetBody`, `DraftFaces` and `ShellBody` in `src/cad_modify.{h,cpp}`.
Thicken is `ShellBody` with nothing opened.

**They are the same operation three times over**, and noticing that is
what made them small. Each changes which surface some faces lie on and
changes nothing about how the body is connected:

- **Offset** moves every face along its own outward normal.
- **Draft** tilts the faces you name about the line where each meets the
  neutral plane.
- **Shell** moves the faces you keep inwards by the wall thickness and
  leaves the ones you open exactly where they are.

After that the corners have to be worked out again -- a corner is
wherever its three faces now meet, which is one 3x3 solve -- and the
edges redrawn between them. But which face touches which, and in what
order, is exactly as it was. So there is one routine underneath,
`RebuildOnSurfaces`, and the three operations are three ways of deciding
what the new surfaces should be.

That is worth stating because the alternative is what a modeller reaches
for once it has a boolean and is pleased with it: build the offset body
separately and subtract. Part E.3 records at length what happens then.
The two bodies touch along whole faces, which is precisely the case the
boolean does not handle.

**Two things a shell gets wrong if they are not said out loud.**

- *The cavity is not the body offset inwards.* With the top opened, the
  cavity has to run all the way up to the opened face rather than
  stopping a wall's thickness short of it -- otherwise the result is not
  a shelled box but a smaller box loose inside a bigger one. That falls
  out of the shared routine for free: the opened face simply keeps its
  original plane while the others move.
- *What is left on an opened face is a rim*, the face with the cavity's
  mouth as a hole in it, exactly `thickness` wide. A sealed shell instead
  carries its cavity as an inner shell -- a void -- which is what Part
  B.1 introduced inner shells for.

**Self-intersection is reported, not resolved.** Offset a box inwards by
more than half its thickness and there is no answer to give. The test is
exact and costs nothing: an edge joins the same two corners it always
did, so if the vector between them has reversed, those corners have
swapped over -- which is what a feature being consumed looks like from
close up. The refusal names the edge.

#### What it handles

Bodies whose every face is a plane and whose every vertex has exactly
three faces at it: every prism, every polyhedron, everything Parts E.1
and E.2 build from a polygonal profile. Everything else is refused by
name. A curved face needs the new edge curves to come from intersecting
the new surfaces rather than from joining the new corners, which is a
table of surface-pair cases belonging with Part C's intersector -- the
same piece of work Part E.3 wants, and not a tolerance away from either.
A body that already has a cavity needs its inner shells re-solved too,
which is this routine run again; a sealed hollow body fed back in is
refused rather than quietly losing its void.

#### Verifying it

Planar faces tessellate without approximation, which is the other reason
to do them first: every check here is exact to 1e-9 rather than to
whatever the chording allows.

    offset  a box by d          (a+2d)(b+2d)(c+2d)
    offset  a prism by d        (A + P d + d^2 sum tan(ext/2))(h + 2d)
    draft   a box by angle t    a b h - tan(t)(a+b)h^2 + (4/3)tan(t)^2 h^3
    shell   a box, sealed       a b c - (a-2t)(b-2t)(c-2t)
    shell   a box, top opened   a b c - (a-2t)(b-2t)(c-t)

**The reflex corner is the one that earns its test.** Offsetting a
non-convex prism with the corners re-solved as plane meetings -- mitred,
not rounded -- grows the area by `A + P d + d^2 * sum over corners of
tan(exterior/2)`, and a reflex corner contributes a *negative* term. An L
has five square corners and one reflex, so that sum is 5 - 1 = 4. A
construction that treated the reflex corner like the others would come
out `2 d^2` too large, so the test also asserts that the answer is *not*
that number.

**Draft's sign is pinned by more than its volume.** The integral above is
symmetric: a box that tapered from the top down instead of the base up
would give the same number. So the test also checks that the base keeps
its size and the top has shrunk, which is the statement that the face
turned about the neutral plane and not about something else -- and it
measures that from the body's own corners, since `Model::Bounds` unions
the surfaces, which are trimmed a hair wider than their faces so a
p-curve cannot land on a domain edge.

**And they compose**, which is the only real test that these leave behind
an ordinary body rather than something only their own code can read: a
box drafted, then shelled, then offset, each step checked against its own
closed form. The last of those is a good worked example of how easy the
arithmetic is to get wrong -- offsetting a tapered side moves its plane
along its own normal, so the section grows by `2d/cos(t)` and not by
`2d`, and the base drops by `d` to where the taper has not yet narrowed
it, which is worth another `2 d tan(t)`. The first draft of that test
omitted the second term and was 2% out. The kernel was right.

### E.3, first attempt: the boolean cutter, and why it is not in the tree

A fillet was built as a *boolean*: make the corner sliver -- the wedge
between the edge and the track a rolling ball leaves -- as a solid of its
own, and subtract it. The appeal was obvious: exact geometry, no new
trim-and-stitch machinery, and Part C.4 already tested on a thousand
random cases.

It does not hold up, and the reason is worth recording because it says
something about the kernel rather than about fillets.

**It worked on a 4x4x4 box and failed on 4x5x4.** Fillets at three radii
and chamfers at two setbacks came out right on the cube -- 63.766 against
63.785, 63.107 against 63.141, chamfers exact to the last bit -- and then
the same code on the same shape one unit longer produced an open shell.
A sweep across sizes found 4x4x4 and 4x8x4 working and 4x5x4, 4x6x4,
4x7x4 and 4x9x4 failing, which is not a bug with a dimension in it. It is
luck.

**The reason is that a corner cutter is made almost entirely of
degeneracies.** Its rounded face is *tangent* to the two faces it trims,
because that is what a fillet is. Its other faces end exactly *on* those
faces, because that is where the material stops. Part C.4 is good at
transversal intersections and poor at everything else, and a cutter like
this hands it nothing else. Offsetting the cutter's straight sides clear
of the body removes the coincidence but not the tangency, and the
tangency cannot be removed without changing the geometry.

So E.3 needed what the plan said it needed: **rolling-ball surface
construction, then trim-and-stitch** -- a local edit of the two faces and
the ones at the edge's ends, not a general boolean. That is what is in
the tree now, and it is described below. Two further things this attempt
established, which that work needed:

- **A concave edge is a different operation, not a sign change.**
  Rounding a convex edge removes material, so the cutter may overshoot
  past the edge's ends into empty space and the answer is unchanged.
  Filling a concave one adds material, so a cutter that overshoots adds it
  beyond the solid's own ends, and one that stops exactly there has its
  end caps lying in the body's -- the coincident-face case C.4 refuses.
- **Convex and concave cannot be told apart from the two face normals.**
  A box's corner and the reflex corner of an L-shaped prism have the same
  pair. What differs is which way each face extends from the edge, which
  has to be read off the face's own boundary.

### E.3, as built: trim-and-stitch

`BlendEdge` and `BlendEdges` in `src/cad_feature.{h,cpp}`, covered by
`mep-cad-feature-test` (384 checks).

Nothing is classified, nothing is intersected, and no surface ever meets
one it is tangent to. The operation edits four faces and adds a fifth:

1. **Read the edge.** Its direction, the two faces' outward normals, and
   -- separately -- which way each face *extends* from the edge, which is
   taken from that face's own boundary rather than from its normal, for
   the reason the first attempt established. Convexity follows from
   comparing the two.
2. **Roll the ball.** Its centre is the point at distance `radius` from
   both planes and square to the edge, which is one 3x3 solve. The only
   difference between a convex and a concave blend is the sign of that
   distance: on the material side for one, in the void for the other.
3. **Trim.** The two faces along the edge lose it and gain the line the
   ball touches them along; the two edges meeting it at each end are cut
   back to where that line crosses them.
4. **Stitch.** The blend surface -- a cylinder for a fillet, a plane for
   a chamfer -- goes in between, and the faces across the edge's two ends
   trade their corner vertex for the blend's cross-section.

**What it handles, and what it says when it does not.** A straight edge
between two planar faces, whose end faces are planar and square to the
edge: every edge of anything prismatic, at any dihedral angle, convex or
concave, on any orientation in space. Everything outside that is refused
by name -- a curved edge, a curved neighbour, an end face at a slant, a
radius that will not fit -- because each of those needs the blend surface
intersected with its neighbours, which is Part C.3's marcher applied to a
surface that does not exist yet. That is a later piece of work and not a
tolerance away from this one.

**Telling "will not fit" from "will not work" costs one paragraph and is
worth it.** `Curve3::ClosestPoint` clamps to a curve's domain rather than
reporting that it ran off the end, so a trim point past the far end of a
neighbouring edge comes back sitting *on* that end and reads as "this
edge does not point the right way" when the truth is "this radius is too
big". Only one of those tells the user to type a smaller number. The trim
does its own arithmetic so the two can be told apart. This is the third
time `ClosestPoint`'s clamping has produced a wrong answer somewhere
rather than a refusal -- see the Part C note above -- and the pattern is
now clear enough to state: **it is the wrong tool for asking whether a
point is on something.**

#### Verifying it

The closed form for a blend is exact, so the test is exact. For an edge
of length L whose faces meet at interior angle `a`, a fillet of radius r
takes away `(r^2/tan(a/2) - r^2 (pi - a)/2) L` and a chamfer of setback s
takes away `(s^2 sin(a) / 2) L`.

- **A sweep of box sizes, not one box.** 4x4x4 through 4x9x4, because
  that sweep is exactly what exposed the first attempt as luck. All six
  now give the same answer to the last digit that the mesh can carry.
- **A sweep of radii**, because a formula that ignored the radius would
  pass a single-radius sweep.
- **A 60-degree edge on a prism tilted away from all three axes.** A
  right angle is the one case where the setback equals the radius, so
  every axis-aligned box test would pass a construction that assumed it.
  This one takes away 3.19 times what a right angle would, and matches.
- **A concave edge on an L**, where the volume goes *up* and the mesh
  reads *over* the true answer instead of under. Both signs flip
  together, so a convexity test the wrong way round fails twice.
- **The rolling-ball property, checked against the B-rep and not the
  mesh**: the blend face is a cylinder of the radius asked for, its axis
  runs along the edge it replaced, it spans exactly `pi - a`, and that
  axis sits exactly `radius` from each of the two planes -- all to 1e-12,
  none of it through the tessellator.
- **That the blend is local**, which is the claim the whole approach
  rests on: the body's extent is unchanged, the far edges are still
  there, and blending a blended body works.

**The volume check is against a derived bound, not a round number.**
Chording an arc of angle `w` into n segments leaves `(r^2/2)(w - n
sin(w/n))` outside the mesh, and the tessellator's refinement rules put a
floor under n, so the deficit has a computable ceiling. The test checks
the volume is never *over* the closed form and never more than that
ceiling under it. The ceiling comes out within 10% of what is actually
measured, which is itself evidence that the difference really is the
chording and not something else.

#### A tessellator defect found while verifying this, and fixed

The blend was first checked at a chord tolerance of 1e-5, and the mesh
came back not watertight. Refinement making a result *worse* is one of
the two diagnostic patterns this plan keeps returning to, so it was run
down rather than tuned around: a plain `MakeCylinder` did the same thing,
watertight at 1e-2 and 1e-3 and coming apart at 1e-4 and below, with 54
unmatched edges all on the seam.

**It was never really about fine tolerances.** Once the cause was
understood it took one search to find a cylinder of radius 10 and height
12 that came apart at the *default* tolerance. The fine settings only
made it easier to hit.

**The cause was the super-triangle.** Bowyer-Watson is usually started by
wrapping the points in a triangle big enough to contain them and throwing
away whatever still touches it at the end. That is wrong, and not
marginally: what has to be contained is not the points but the
*circumcircles*, and those are unbounded relative to the point set's
extent. One sliver -- two points a hair apart and a third far off -- has
a circumradius of `distance^2 / (4 * hair)`, so no fixed multiple of the
extent is safe. When the super-triangle is too close, the algorithm
correctly builds triangles onto it, and the final "discard the
scaffolding" step then punches holes in the middle of the result.

Slivers like that are not unusual here; they are *structural*. A
cylinder's seam is a straight line, so the edge sampler gives it two
points, while the face's interior is gridded by curvature into a hundred
rows. The strip between the seam and the first interior column is
therefore spanned by triangles whose circumradii are a hundred times the
face's own size. The super-triangle sat at twenty times the extent, well
inside them.

**The fix has no constant in it.** The super-triangle is gone and there
is a single extra vertex at infinity. A ghost triangle `(a, b, INF)`
stands for the half-plane beyond hull edge `a->b`, and a point conflicts
with it when it lies on the far side -- an Orient2D test instead of an
InCircle one. Every triangle, ghost or real, is stored with its own
region on its left, which is what makes the cavity's edges cancel in
pairs the same way for both. A point landing exactly on a hull edge is
handled by the ghost and the real triangle behind it conflicting
together, so their shared edge cancels and no triangle of zero area is
ever built.

Reduced to a minimal case -- a left side carrying only its two endpoints
against an interior grid -- the old code failed at twenty points, and the
number of holes grew with the row count. The new one is exact at every
size tried.

**What was measured and rejected.** The scan is quadratic: it asks every
live triangle about every point, which is the price of not depending on
the conflict region being connected -- and a subtly wrong connectivity
assumption is precisely the family of bug the ghost vertex is there to
prevent. Caching each triangle's circumcircle to reject distant points
cheaply made it *slower* (13 minutes against 7 and a half on the worst
case), because `InCircle` already carries its own floating-point filter
and the extra fields only cost memory traffic. What did help, by two
times and with bit-identical output, was compacting dead triangles
instead of re-reading the whole history of the triangulation at every
insertion. The quadratic remains, and is the reason the blend tests mesh
at 3e-4 rather than finer: past there the check stops getting usefully
sharper well before it stops being affordable.

Regression test in `mep-cad-topology-test`: five cylinders confirmed to
fail before and pass after, the first of them at the default tolerance,
plus the statement that actually broke -- that refining the mesh
converges upward rather than away.

### Kept from that work

- **`Feature::EdgeReference`**: an edge named by the two faces it lies
  between. Part B.4 will not guess -- it reports a reference it cannot pin
  down as ambiguous rather than picking one -- and a box's twelve edges
  are alike in most of what it scores on, while its faces are not. E.3
  will refer to its edges this way.
- **Every side face gets its own role** ("side 0.2" rather than "side"),
  because a face's role is one of the strongest signals B.4 has and
  giving every side of a block the same one throws it away.

### Two more gaps in Part C found by Part E.3's attempt

- **`ClipLineToSurfaces` sampled the line over a window sized by the
  whole model**, with a fixed sample count, so an overlap shorter than one
  sample interval was never seen rather than merely unrefined. A fillet's
  cutter is a fraction of the size of the part it cuts, and its faces met
  the part's in runs that fell between samples. The window is now the
  overlap of the two surfaces' own bounding boxes.
- **The boolean's trimming took "on this face" to mean the point-in-face
  test alone.** `ClosestPoint` clamps to a surface's parameter range
  rather than failing, so a point beyond a surface's extent comes back
  sitting on its boundary, where the in-face test decides by round-off.
  A surface trimmed to exactly its own face -- which an extruded side
  face is -- makes that the common case. The trimming now checks that a
  point really lands on the surface.

### What remains in Part E

- [x] **E.3 Fillets and chamfers.** Constant-radius rolling-ball fillets
  and flat chamfers on straight edges between planar faces, by
  trim-and-stitch, convex or concave, at any dihedral angle. Described
  above.
  - [ ] Still to do, and each needs the blend surface intersected with
    its neighbours rather than stopped against them: curved edges,
    curved neighbouring faces, end faces at a slant, variable radius by a
    law along the edge, and vertex blends where three filleted edges
    meet. `BlendEdges` carries several edges across the rebuilds by the
    face pair each lies between, so edges that do not touch can be
    blended in one call; edges that meet at a corner cannot, and say so.
- [x] **E.4 Shell, draft, offset and thicken.** `src/cad_modify.{h,cpp}`,
  covered by `mep-cad-modify-test` (230 checks). Planar faces only;
  described below.
- [ ] **E.4 (rest).** Curved faces, which need the surface-pair
  intersection table above; bodies that already carry a void; a
  variable-thickness shell, where different walls take different
  thicknesses; and offsetting a face set rather than a whole body.
- [x] **E.5 Patterns and transforms.** `src/cad_pattern.{h,cpp}`, covered
  by `mep-cad-pattern-test` (148 checks). Linear, grid, circular, mirror,
  sketch-driven and table-driven, separate or merged. Described below.
  - [ ] Patterning *faces* rather than whole bodies, which is local
    surgery of the Part E.3 kind rather than a transform and a copy.
- [x] **E.6 Datums and assemblies.** `src/cad_assembly.{h,cpp}`, covered by
  `mep-cad-assembly-test` (213 checks). Datum points, axes and planes;
  instances and mates; the solver is Part D.2's engine lifted to three
  dimensions, sharing its dual numbers through the new `cad_dual.h`.
  Described below.
  Sub-assemblies, mates that survive a rebuild, and parts driven by a
  feature tree are all in.

---

## Part F — Interchange

**Status: complete.**
`src/cad_doc.{h,cpp}`, `src/cad_step*.cpp`, `src/cad_exchange*.cpp`,
`src/cad_drawing.{h,cpp}`, and `Mode::Cad` in the editor.

- [x] **F.1 Native `.mepcad`.** `src/cad_doc.{h,cpp}`, covered by
  `mep-cad-doc-test` (173 checks). The feature tree plus sketches plus
  persistent-name data, as text, versioned. Described below.
- [x] **F.2 STEP read.** `src/cad_step.h`, `src/cad_step_parse.cpp` and
  `src/cad_step_read.cpp`, covered by `mep-cad-step-test` (162 checks).
  The Part 21 grammar in full; the B-rep entity subset. Described below.
  - [ ] Still to do: the geometric-tolerance and product-structure
    entities, so that a STEP assembly comes in as a Part E.6 assembly
    rather than as its bodies.
- [x] **F.3 STEP write.** `src/cad_step_write.cpp`. AP214 out, analytic
  surfaces preserved as analytic, with the minimum product structure so
  that a receiver can find the shape at all. Verified by round trip
  through the reader *and*, since Part G.7, by OpenCASCADE 7.8 reading it
  through Gmsh and meshing bodies of the right volume — which is what
  found the missing product structure. See G.7 below.
  - [ ] Still to do: an assembly out, as a product structure with more
    than one part in it. One part is written now; several is Part E.6's
    tree and is not the same job.
- [x] **F.4 The rest.** `src/cad_exchange.h` with `cad_exchange_mesh.cpp`,
  `cad_exchange_dxf.cpp` and `cad_exchange_iges.cpp`, plus
  `src/cad_drawing.{h,cpp}`; covered by `mep-cad-exchange-test` (91
  checks). STL/OBJ/glTF out, DXF both ways, IGES in, and drawing views
  with hidden-line removal, written as SVG. Described below.
  - [ ] Still to do: drawing views into `src/pdf_writer.cpp` rather than
    only SVG, and IGES type 144 (trimmed surfaces), which is what most
    IGES exporters actually produce.
- [x] **F.5 `Mode::Cad` and `CadSession`.** In `editor.{h,cpp}` and
  `main.cpp`, with `mep.cad_*` Lua functions, `cad.*` agent-RPC methods
  and `mep-cad-live-test` driving a real running mep. Described below.

---

### F.1: the only format that stores the operations

`WriteCadDocument` / `ReadCadDocument` and `WriteSketch` / `ReadSketch`
in `src/cad_doc.{h,cpp}`.

**What is not stored is the point.** There is no geometry in a `.mepcad`
file -- no faces, no edges, no surfaces. Reading one gives back a
`FeatureTree` that has not been evaluated; calling `Rebuild` on it
reproduces the body. Every other format mep will read or write carries
the faces a feature tree produced but not the tree, so opening one and
changing the extrude distance is not a thing that can be done.

It is JSON, through the in-house parser already in the tree, because
`fem_model` persists that way and a second hand-rolled format would be a
second thing to get wrong. Enumerations are written as *names*, never as
numbers: a number is smaller and silently changes meaning the day a kind
is inserted in the middle of an enum, which is the sort of change nobody
thinks of as a format change until a year-old file opens as the wrong
shape.

**Versioned from day one**, because the alternative is discovering on the
day of the first change that there is no way to tell an old file from a
new one. The version is the first thing written and the first thing
checked, and a file from the future is refused by name rather than
half-read.

Reading a sketch has to restore it *exactly*, ids included, since its
constraints refer to its points and entities by id and a sketch whose
constraint names geometry that is not there is not solvable at all.
Replaying the `Add` calls in order would very nearly work and would break
the first time a kind allocated an id differently, so `Sketch::Restore`
puts the arrays back directly and says in the header that it is doing so
rather than pretending it went through the front door.

#### Verifying it

**The comparison is made on rebuilt geometry, not on the text.** Writing
a document and reading it back to an identical document proves only that
the writer and the reader agree with each other. So the test builds a
part -- a plate, a hole cut through it, a boss added on top -- saves it,
loads it, rebuilds *from scratch*, and compares the volume and the full
topology counts against the original. It also checks the loaded tree has
no geometry until it is replayed, which is the claim the format exists to
make.

**Then it writes what it read and requires the same bytes.** That is the
check that catches a field the reader quietly drops and the writer then
supplies a default for -- the failure where the first round trip passes
and the second is where the loss shows.

**Then it changes a dimension in the reopened document** and requires the
same result as changing it in the original, because a format that stores
operations is only worth having if the operations still work.

The sketch is tested separately and exhaustively -- points, lines, arcs
with their sense, circles, ellipses with a fixed scalar, splines with
their knots, driving and reference dimensions, and an attached plane
carrying its Part B.4 name -- because a sketch is the part of this with
ids referring to other ids, and testing it on its own is what makes a
failure in the whole document easy to place.

One detail the test had to work around rather than expose: the sample
part's boss starts *inside* the plate rather than flush with its top
face, and the "change a dimension" step thickens the plate to 4 rather
than 5. Flush in either case would put two coincident faces in front of
the union, which is Part C.4's known limitation and not something this
test exists to rediscover.

---

### F.2 and F.3: STEP, in and out

`src/cad_step.h`, with the physical file in `cad_step_parse.cpp`, the
entity mapping in `cad_step_read.cpp` and the writer in
`cad_step_write.cpp`.

**Two layers, deliberately separate.** ISO 10303-21 is a small regular
grammar of entity instances, lists, strings, enumerations and references,
and it knows nothing about geometry; mapping those entities onto the
B-rep is a different job. Keeping them apart matters because almost every
problem with a real STEP file is in the second layer: the file parses and
then says something the reader was not expecting. A parser with geometry
mixed into it would report those as syntax errors.

**Three conventions are where the work is**, and all three are places a
reader goes wrong quietly rather than loudly:

- *An edge has two orientations and they multiply.* An `EDGE_CURVE`
  carries a sense against its own curve; an `ORIENTED_EDGE` carries
  another against the `EDGE_CURVE`. A loop traverses the edge forwards
  only when the two agree, and honouring one and forgetting the other
  produces faces whose boundaries run backwards -- which validates
  structurally and tessellates inside out.
- *A face has its own*, and a `FACE_BOUND` has yet another on top of the
  loop inside it.
- *Parameters are not normalised.* A STEP edge carries no parameter range
  of its own, so it has to be worked out from the vertices. Assuming is
  how a reader ends up with arcs that run the long way round.

The writer keeps analytic surfaces analytic -- a cylinder goes out as a
`CYLINDRICAL_SURFACE`, not as a B-spline that happens to be round -- and
that is not a nicety. The receiving system's own fillet, draft and shell
operations all depend on knowing a face is a cylinder, and a file that
has thrown that away can only be tessellated by whoever gets it.

#### Verifying it

The round trip is the only oracle available without a corpus of other
people's files, and it is a real one provided the comparison is on the
*geometry*: write a solid, read it back, require the same volume, the
same topology counts, and a body that still validates and still
tessellates watertight. A writer and a reader that agreed with each other
and with nothing else would pass a text comparison and fail this.

The shapes are chosen for what the mapping has to get right: a box for
planes only, a cylinder for a seam, a sphere for poles, a torus for
periodicity in both directions, a Part E.3 fillet for an analytic surface
this kernel produced, and a hollow box for `BREP_WITH_VOIDS` rather than
`MANIFOLD_SOLID_BREP`.

The parser is tested separately and adversarially, because the round trip
only exercises the corner of Part 21 that mep's own writer emits: every
kind of argument, comments mid-record, doubled quotes inside strings,
complex instances, entities out of order with forward references. And
there is a unit cube written by hand in another system's style --
different spellings, everything referring forwards -- which is the
closest thing to a foreign file available without shipping one.

#### Three kernel defects this turned up

All three were found by the same shape of failure: a sphere read back
from a file described the same arc with its parameters a full turn along,
which is legal and is what recomputing an edge range from its vertices
naturally produces.

- **The validator asked whether a point was on an edge using
  `Curve3::ClosestPoint`**, which clamps to the *curve's* declared domain
  rather than searching the *edge's* range. Those are not the same
  interval and on a periodic curve they need not overlap. This is the
  fourth place in this kernel where that clamping has produced a wrong
  answer rather than a refusal, and the rule is by now plain: it is the
  wrong tool for asking whether a point is on something. Fixed by
  searching the edge's own range.
- **A p-curve sample sitting exactly on a degenerate point** -- a
  sphere's pole -- has one parameter that means nothing, and the
  inversion returned whichever value the last bit of a cosine happened to
  give. The two descriptions of the same arc gave different ones, and the
  seam projected to a different place in parameter space. Fixed by taking
  the free parameter from the nearest sample that is not degenerate, and
  by working out *which* parameter is free from whichever derivative
  vanished rather than assuming.
- **`Curve3::Tessellate` works over the curve's domain**, so filtering
  its output to the edge kept samples for the part that overlapped and
  none at all for the part that did not -- leaving half the edge as one
  straight chord. That is how a sphere came back a percent short of the
  one it was written from while validating perfectly and being
  watertight. Fixed by sampling the edge's own range at the density the
  curve asked for.

### F.4: the rest of interchange

**The tessellated formats are all the same format.** STL, OBJ and glTF
differ in how they spell a triangle and in nothing else, so they share
Part B.5's mesh. What is worth being careful about is what they lose:
every one throws away the faces, the surfaces and the topology and keeps
an approximation. They are for looking at and for printing.

Each is checked by reading its output back and measuring it -- STL
through its own reader, OBJ and glTF by recovering the geometry from the
text and requiring the declared counts and buffer lengths to agree with
what is actually there. The things that get got wrong are checked
explicitly: OBJ indexes from one, and a binary STL must not begin with
"solid" or every reader in the world takes it for the ASCII form.

**DXF goes both ways and loses the constraints**, which is stated in the
header because it decides what it is for: DXF has no way to say "these
two lines are perpendicular", so a sketch written out and read back is
the same geometry and no longer parametric. `.mepcad` is the format that
keeps them.

**IGES is read and not written.** It is still common in supply chains, so
files arrive; nothing is improved by sending one back when STEP says the
same things better. It is a punched-card format -- eighty columns, a
section letter in column seventy-three -- and its pointers are *directory
line numbers*, which are odd because each entry takes two lines. Getting
that wrong is the classic IGES reader bug. The test writes a tetrahedron
out by hand, which is what forces the layout and the pointer arithmetic
to be right rather than assumed; the first version of that test truncated
a 66-character record at 64 columns, which is exactly the continuation
case a real file hits constantly and the reader now demonstrably handles.

**Drawing views do hidden-line removal by sampling**, and the compromise
is stated rather than hidden. The exact method -- intersect every edge
against every face's silhouette in the projection plane -- is right for a
finished drawing and is a substantial piece of work of its own. What is
here cuts each edge into pieces and tests each piece's midpoint against
the tessellated body along the view direction: exact in the only sense
that matters (a piece is hidden or it is not) and inexact only in *where*
the transition happens, to within one piece. It is good for a view to
look at, check and print, and not for a dimensioned drawing whose line
ends are trusted to the micrometre.

Checked against things that are true of a drawing rather than against a
picture: a cube seen square-on has four edges visible and four hidden
behind them, from a corner only the three meeting the far corner are
hidden, and a cylinder's outline contains silhouettes that are not edges
of the body at all. Edges pointing straight at the viewer project to
points and are dropped, because a drawing does not draw those -- leaving
them in gives a cube with twelve lines, four of them invisible.

### F.5: the CAD pane

`Mode::Cad` and `CadSession` in `editor.{h,cpp}`, drawn in `main.cpp`,
with `mep.cad_*` Lua functions and `cad.*` agent-RPC methods over exactly
the same `Editor::Cad*` surface -- the rule this whole part has been
built on since Part D.5.

**The document is the feature tree and the body is derived from it.**
That is the whole difference between this pane and `Mode::Model3D` next
door: there the mesh *is* the document and editing means moving vertices;
here the document is a list of operations and the geometry is what you
get by replaying them. A part read from STEP or IGES has no tree, so the
pane says so rather than letting the user find out by trying to change a
dimension.

**The shading is done in software**, deliberately. A CAD part is a few
thousand triangles that change only on rebuild, so there is nothing to
gain from uploading it to the GPU and a good deal of lifecycle
bookkeeping to lose -- a mesh handle per buffer, freed at the right
moment, reuploaded on every edit. Projecting and sorting on the CPU costs
less than that and keeps this pane, like every other in-pane viewer,
free of graphics state. Painter's algorithm after back-face culling:
exact for a convex solid and wrong only where two triangles
interpenetrate, which a valid B-rep's tessellation does not do.

Verified by `mep-cad-live-test`, which drives a real running mep over its
agent socket: build a sketch, build a part on it, check the volume is the
180 a 10x6 plate 3 thick should be, change the distance to 7 and check it
is 420, suppress the feature and check the body goes away, write all six
export formats and check each is readable, reopen the `.mepcad` and check
it is *still parametric* by changing the dimension again, and reopen the
STEP and check it comes back as geometry that says so.

Two contrast bugs were found by looking at a screenshot rather than by
any test, and both were the same mistake: text drawn in `Normal` on a
background that was not `NormalBg`. The sidebar borrowed the status
line's background, and the selected row is drawn on `VisualBg` and needs
`Visual`. A theme chooses those pairs to go together; taking one from
each pair is how light text ends up on a light panel.

---

## Part G — Meshing: the CAD→FEM bridge

`src/fem_mesh.h` with `fem_mesh_size.cpp`, `fem_mesh_surface.cpp` and
`fem_mesh_volume.cpp`, covered by `mep-fem-mesh-test` (81 checks). This
part is what makes the coupling real rather than a file handoff.

**Known defect, found in Part H.6 and diagnosed but not fixed: the
mesher's quality depends on the size of the part.** The same cylinder at
radius 0.5, 2 and 5 meshes with a worst dihedral angle of 15 degrees and
no slivers; at radius 0.05 -- an ordinary size for a machined part -- it
gives 0.05 degrees and twenty-five slivers, and Part G's own cylinder test
shrunk forty times goes from no slivers to a hundred and twenty.

The first guess was an absolute length where a relative one belonged, and
**that guess was wrong**; it is recorded because the disproof is the
useful part. The surface meshes at two scales have *identical* node
positions, agreeing to 9e-14 relative, so nothing is placing nodes
differently. What differs is the connectivity: 238 of 1240 triangles.

The cause is round-off in a nearly degenerate Delaunay input. A cylinder's
cap is a disc, so its boundary nodes are cocircular in intent and not in
fact -- they come from evaluating a circle at parameters, so they lie on it
to within the last bit. The exact predicate then answers correctly for the
points it was actually given, and that answer differs between scales
because the round-off does. There is no tie to break: a *nearly*
cocircular point set has many valid Delaunay triangulations and some of
them contain slivers.

So the fixable half is the refinement pass, which should remove such a
triangle whichever triangulation produced it, and cannot: Ruppert's
algorithm answers an encroaching circumcentre by splitting the boundary
segment, and a face cannot split a segment it shares with its neighbour
without the two disagreeing about the nodes along their common edge. **The
fix is to refine the edge discretisation globally, before any face is
meshed**, so that both faces see the same split. That is real G.2 work.

Along the way the 2D triangulator got the same symbolic tie-breaking the
tetrahedraliser has, since exactly cocircular points do occur -- a lattice
on a planar face is cocircular four at a time -- and it is worth having
whether or not it addresses this. It does not, and the comment there says
so.

**Status: Part G is complete and verified -- `MeshBody` produces a
watertight volume mesh whose elements add up to the body exactly and
which carries no slivers at all, on a box, on a cylinder and on a
non-convex L. Worst dihedral angle, no sliver count and a positive
scaled Jacobian are gated by the test rather than printed. G.5 curves
the mid-side nodes onto the exact B-rep, which takes a sphere's volume
error from 5% to 0.03%. G.6 sweeps a prismatic body into hexahedra whose
volumes add up to the body exactly, including profiles with holes and
reflex corners, and refuses the bodies it cannot do. G.7 measures all of
it against Gmsh, on geometry carried across as STEP.** What is
missing and why is below, at the point where it bites.

- [x] **G.1 Sizing field.** A background octree carrying target element
  size, seeded from curvature, from thin-wall detection and from local
  overrides, graded by a Lipschitz condition. Described below.
  - [x] The error estimator that seeds it in later adaptive passes
    arrived with J.5, through `SizingOptions::refinements`.
- [x] **G.2 Surface meshing in parameter space.** Shared edges meshed
  once, then a metric-spaced interior seeding and Delaunay refinement in
  (u,v) under the first fundamental form. Described below.
- [x] **G.3 Tetrahedralisation.** Bowyer–Watson Delaunay on the surface
  mesh's nodes with ties broken symbolically, interior points laid down
  to the sizing field, and the boundary read off the elements that were
  kept. Predicates from 0.2 throughout. Described below.
  - [ ] Schönhardt-like bodies -- ones no tetrahedralisation of their own
    boundary nodes can fill -- still need true Steiner recovery. They are
    reported by the volume not adding up, not silently meshed wrong.
  - [ ] Classification is one ray test per element against the surface
    triangles, which is quadratic. It wants a grid or a BVH before G.7
    meshes a real STEP corpus.
- [x] **G.4 Quality improvement.** Laplacian smoothing between two
  rounds of topological flipping (2-3, 3-2, 4-4). Quality reported as
  min/max dihedral angle, aspect ratio, scaled Jacobian — and gated: a
  mesh below threshold is reported, not silently solved. Described below.
  - [ ] Optimisation-based smoothing, which moves a node to where the
    worst element around it is best rather than to the average of its
    neighbours. Laplacian plus flipping clears every sliver on the
    bodies tested, so this is not yet earning its complexity.
- [x] **G.5 Curved second-order elements.** Tet10 and Tri6 mid-edge nodes
  projected back onto the *exact* B-rep — onto the curve where the edge
  lies on one, onto the surface where it lies on a face — with validity
  checking against the quadratic map, not the corners. Described below.
  - [ ] `ToAnalysisModel` still carries neither Tet4 nor Tet10 into the
    solver; that is Part H.1's element library, not this.
- [x] **G.6 Structured and hex meshing.** Sweep meshing for prismatic
  bodies: the profile quadrangulated by subdivision, smoothed once, then
  copied up the sweep in layers. All hexahedra, no wedges, for any
  profile including ones with holes and reflex corners. Described below.
  - [ ] Boundary-layer prisms, which are a different thing: graded layers
    *inside* a tetrahedral mesh against a wall, for when Part I's
    convection or a later flow solver needs the gradient resolved.
  - [ ] Sweeping along a curved path, and sweeping between two faces that
    are parallel but not congruent (a taper). Both are cases the checks
    here deliberately refuse rather than approximate.
  - [ ] Hex20, the second-order hexahedron. Part G.5 curves tetrahedra;
    the same argument applies here and the same machinery nearly does.
- [x] **G.7 Verification.** Every generated mesh checked for positive
  Jacobians everywhere, closed boundary and consistent orientation; and
  element quality and node counts compared against Gmsh's Delaunay and
  Gmsh's Netgen frontal algorithms on an identical corpus, carried across
  as STEP written by this library. `src/fem_mesh_compare_test.cpp`, run
  by `just test-mesh-compare`. Described below.
  - [ ] A corpus of real parts rather than four synthetic ones. The four
    are chosen to cover the cases (flat, curved, closed, non-convex) and
    they are still four.

### G.1: three things seed a size, and a fourth spreads it

`SizingField` in `src/fem_mesh_size.cpp`. Curvature, thin walls and local
overrides, and the reason there are three is that each catches something
the others cannot: curvature puts elements across a fillet and says
nothing about a flat plate two millimetres thick; thin-wall detection
says nothing about a fillet, since a fillet is not thin; and neither
knows that the user wants *this* face finer.

Then gradation, because a field seeded from a small feature and left
alone jumps straight back to the target one cell away -- which puts a
badly graded element exactly where the interesting geometry is.

**Four things went wrong, and each of them is a lesson rather than a
slip.**

- *A seed only covers where it is put.* The curvature pass sampled each
  face on a fixed 7x7 grid, which on a cylinder twenty units long is
  three units between samples asking for a size of a third of a unit. The
  field ended up with forty-nine fine cells and coarse everything
  between. Both the curvature and thin-wall passes now sample at a
  density set by the size they are about to ask for, which needs two
  passes: one to find out what the face wants, one to lay it down.
- *A closed surface's span is not the distance between its ends*, which
  are the same point. Measuring a cylinder's circumference that way gave
  zero, the sampling collapsed to three angles, and the field was seeded
  at three points around a circle. Spans are walked now, not subtracted.
- *Subdividing an octree cell must not hand the new size to all eight
  children.* Driving a size down lowered every cell on the way, and the
  children created en route inherited it -- so one 0.1 refinement in the
  middle of a box made the entire box 0.1. Only the leaf the point lands
  in takes the new size; gradation is what spreads it, which is the whole
  point of having a gradation pass.
- *Gradation per cell is not gradation.* "Neighbouring cells differ by at
  most `growth`" sounds right and is not: how fast the size then grows
  depends on how finely the octree happens to be divided there. The
  condition is Lipschitz -- at most `growth - 1` per unit travelled --
  which is a statement about the model rather than about the data
  structure. And since the stored field is piecewise constant, `At` does
  not return the cell's value but the Lipschitz envelope of the
  neighbourhood, which is the continuous field the gradation promised.

Two things that had to be got right to enforce that: a cell's face
neighbours are *enumerated*, by asking which leaves overlap a thin slab
just outside each face, because probing the face at a few points finds
some of a coarse cell's dozen fine neighbours and misses the rest -- and
the ones it misses are the ones that never get graded. And the distance
in the envelope is to the cell's *box*, not to its centre; using the
centre makes even a uniform field read back as something other than
itself.

Verified against what the field is for rather than against itself: a
cylinder of radius 1 at 0.35 radians per element reads exactly 0.35 and
one of radius 4 reads exactly 1.40, a plate one unit thick asking for two
elements through it reads exactly 0.5, and a 0.15 refinement in the
middle of a forty-unit box grades out to the four-unit target without
ever breaking the Lipschitz bound.

### G.2: meshing a face in its own parameters

`MeshSurface` in `src/fem_mesh_surface.cpp`.

**Why this cannot be a generic 2D mesher.** A face is meshed in its own
(u, v), because that is where its boundary is a polygon. But an element
well-shaped in (u, v) is not well-shaped in space: a cylinder's
parameters are an angle and a height, so a square in parameter space is a
rectangle of aspect ratio r on the surface. Every length and angle is
measured under the metric the surface induces on its parameters -- the
first fundamental form -- and that is the whole difference.

**Shared edges are meshed once**, before any face is touched, so two
faces meeting along an edge get the same nodes on it. A per-face mesher
that does not do this produces a surface with cracks that no tolerance
will close.

**Two departures from Ruppert's algorithm, both forced and both worth
stating.**

- *Boundary segments are never split.* Ruppert splits an encroached
  segment, which is what guarantees both the angle bound and the
  boundary's presence in the triangulation. But a segment here lies on a
  CAD edge shared with the face on the other side, and splitting it on
  one side only leaves the two faces with different nodes along their
  common edge. That was found the direct way: the box meshed to exactly
  the right volume and did not close. The edges are discretised to the
  sizing field up front instead, and a refinement point that would
  encroach one is dropped.
- *Refinement happens in passes.* Implemented literally -- find the worst
  triangle, split it, repeat -- every inserted point re-triangulates the
  whole face, which is cubic and does not finish. Each pass collects
  every triangle that wants splitting and inserts all the circumcentres
  that are far enough apart to be worth inserting.

**And the interior is seeded with a grid first.** Refinement alone gets
there on an easy face and does not get there at all on a hard one: a
sphere is one face whose entire boundary is its seam, used twice, so the
starting triangulation is two parallel chains of points with nothing
between them, every triangle spans the whole face, and the circumcentres
of such triangles land outside it. A metric-spaced grid turns every face
into the easy case.

Verified by closure and volume, which together are what a surface mesh
has to get right: a box meshes to exactly 240 at two densities, a
cylinder to 98% of its true volume, a sphere to 95%, and all four close.

### G.3: two ties and a coin flip

`Tetrahedralise` in `src/fem_mesh_volume.cpp` -- Bowyer-Watson with a
vertex at infinity, the same formulation the 2D tessellator uses and for
the same reason recorded there.

**It was wrong three times, and each symptom was unmistakable while each
cause was not.**

*Orientation.* Two hundred random points produced 328457 tetrahedra with
fifty thousand faces belonging to more than two of them, which is not a
mesh of anything. `Orient3D`'s sign convention is Shewchuk's -- positive
when d lies *below* the plane abc -- which is the opposite of the right
hand rule the volume formula uses, so a tetrahedron with positive
`Orient3D` has negative volume by that formula. Face windings derived
from one convention and tested with the other are wrong half the time,
and a cavity whose faces do not cancel is refilled with overlapping
tetrahedra. Nothing derives an orientation now: every tetrahedron and
every face is ordered by asking `Orient3D`.

*The ties, which are not the exception.* A cavity is only star-shaped
when no two competing answers are exactly tied, and on the inputs a
mesher actually sees the ties are everywhere: a planar face's nodes are
exactly coplanar -- that is what planar means -- and a regular lattice is
cospherical eight points at a time. Moving the points by a hair fixes the
ties and breaks the mesh, because boundary nodes stop being coplanar.

So the points stay where they are and the tie is broken where it is a
tie: point *i* is lifted onto the paraboloid by ε^i more than its
neighbours, which is a regular triangulation with weights too small to
hide any point and which has no ties at all. `InSphere` is the
determinant of the lifted points, so raising one point's lift moves that
determinant by the cofactor of its entry, which is up to sign an
`Orient3D` of the other four -- so when `InSphere` is exactly zero the
answer is the first non-zero such `Orient3D`, taken in order of the
dominant point. **The signs were measured, not derived**: a scratch
program evaluated the 5x5 determinant directly and compared, which took
minutes and would have taken hours by hand with no way to be sure.

The hull needs the same care and gets it differently. A point exactly
coplanar with a hull facet is beyond it exactly when it falls inside that
facet's circumcircle -- the 2D Delaunay question, asked within the plane.
That needs no second predicate: a sphere through the facet's three points
cuts their plane in their circumcircle whatever the fourth point is, so
for a point already in that plane "inside the facet's circumcircle" and
"inside the neighbour's circumsphere" are the same question, and the
coplanar ghosts are decided from which real tetrahedra died. That also
makes the two tests consistent by construction rather than by argument,
and consistency is the whole of what the cavity needs.

Verified on three point sets, and the middle one is the point: 200 random
points, a 5x5x5 lattice where every eight neighbours are cospherical, and
a plane of points with two above it -- which is exactly what one flat
face of a body looks like. The lattice gives 384 elements, which is six
per cube of a 4x4x4 grid, and 192 boundary faces, which is the cube's
hull triangulated. All three give a partition: no face in more than two
elements, no element with negative or zero volume.

**The boundary is the surface mesher's nodes, not its triangles.** A
Delaunay tetrahedralisation of those nodes need not contain those
triangles, and on a planar face it usually does not. Recovering them with
Steiner points was written first and it *diverged*: five thousand points
left nine thousand triangles still missing on a cube, because splitting a
coplanar triangle adds another coplanar node and another tie. A different
triangulation of the same nodes spans the same polygon on a planar face
and interpolates the same points on a curved one, so the boundary is read
off the elements that were kept -- every face used by exactly one of them
-- which is watertight because a face of a tetrahedralisation belongs to
one element or two and there is no third possibility.

**Which elements are kept is one ray test each, and the cheaper thing was
wrong.** The cheaper thing was to build a barrier from the faces that lie
on the body, let the elements fall into regions that cannot cross it, and
decide each region once. A barrier is built by asking whether a face lies
on the body, and where the boundary turns a reflex corner the honest
answer for every candidate is no, because the tetrahedralisation cuts the
corner rather than turning it. The barrier has a hole, inside and outside
join into one region, and the single test that decides it is made deep in
the material and says inside: on an L-shaped block that kept half the
notch, 224 of volume where the body has 192. Sampling the region at
several of its elements does not help -- its biggest ones are all in the
material and all agree.

**And the ray test itself was guessing.** It counted crossings in three
directions and returned *outside* when all three passed too near an edge
to be trusted. On a structured mesh that is not the rare case: a box's
triangles are coplanar in sheets and share edges in rows, so a fixed
direction grazes something almost every time, and whole interiors were
called outside. It has eight directions now and, beneath them, a test
that cannot graze: the nearest point on the surface and which side of its
triangle the point is on. Per element there is then no barrier to leak
through, and the L-shaped block measures 192.0000 against 192.

A fourth diagnostic pattern to go with the three above: **a mesh that is
watertight and has the wrong volume has been classified wrong, not built
wrong** -- the elements are a partition either way, so only the
inside/outside decision can have moved the total. It is the check worth
making because an element count, a validity check and a closed boundary
all pass while it is wrong.

### G.4: a sliver is flipped away, not deleted

Quality is measured and is calibrated against the one number in this part
that can be written down from first principles: a regular tetrahedron's
six dihedral angles are all `arccos(1/3)`, which is 70.5288 degrees, and
its aspect ratio and scaled Jacobian are both exactly 1. A sliver -- four
nearly coplanar points -- reads 0.14 and 179.80 degrees and is counted as
one. `CheckMesh` refuses a mesh with an inverted element, a node that
does not exist, a face in more than two elements or a boundary that does
not close, each by name.

**A sliver cannot be deleted -- that leaves a hole -- and it cannot be
smoothed away either, because its four nodes are where they should be.
It is the connectivity that is wrong.** So the region it sits in is
re-triangulated, which changes no node and moves no boundary. Three flips
cover every re-triangulation of a small region that keeps its outer
faces: 2-3 (two elements sharing a face become three sharing a new edge),
3-2 (its inverse) and 4-4 (four elements around an edge become four
around another, the region being an octahedron and the flip picking a
different one of its three diagonals).

Two guards make this safe without a convexity test of its own. **The
volumes have to match**: the pieces put back are summed and compared with
the pieces taken out, so a flip that would overlap or leave a gap is
refused, and the non-convex cases where a flip is simply not available
refuse themselves. And **the worst element it touches has to come out
better than it went in**, so nothing here can make a mesh worse; the most
it can do is nothing. The boundary is safe by counting: a 2-3 needs two
elements on the face it opens, and an edge closed by a 3-2 or a 4-4 is
interior by the same count.

Flip, then smooth, then flip again. The two do different jobs and each
makes work for the other, and the second pass is where most of a
cylinder's slivers go. Smoothing is Laplacian over the interior nodes
only, and a move is kept only if every element it touches stays valid --
smoothing that inverts an element has made things worse however much
rounder the node looks. Boundary nodes are never moved.

**And then the last slivers turned out not to be a G.4 problem at all.**
With flipping in, the L-shaped block still had 96, and 91 of them had all
four nodes on the boundary -- locked, because every flip that would help
would move the boundary. That is a real limit of flipping, but it was not
the cause. The cause was that the block's surface mesh had 4420 triangles
where its area at the target size wants about 200, so the body was a
shell of surface nodes with sixteen interior points behind it and no room
for anything but flat elements.

The sizing field was doing that, and for a reason worth recording.
**Thin-wall detection shoots a ray into the material and measures how far
it is to the far side, and a sample sitting exactly on a reflex edge
shoots along the plane of the face that meets it there.** A ray coplanar
with a triangle is the degenerate case of a ray-triangle test and reports
a hit at very nearly zero distance -- a wall of no thickness, which
drives the size to its floor. Three samples out of thousands did it, and
those three multiplied the mesh twentyfold. The guard is that **the far
side of a wall faces back at you**: the hit triangle's normal must point
back along the ray, which a real far side does easily and a grazed face,
being edge-on, cannot. With that, the block meshes to 444 elements with
no slivers instead of 6041 with 96.

A fifth diagnostic pattern, then, to go with the four above: **when a
mesh is bad everywhere, suspect the sizing field before the mesher.** The
mesher is what one reads first because it is what one just wrote, and
element counts far above what the target size implies are the cheap check
that says to look upstream instead.

- [x] **Slivers with all four nodes on the boundary** (the item that stood
  here) are now repaired, by letting a boundary node slide along the CAD
  face or edge it came from rather than freezing it. See the sweep note
  below.
- [ ] Still to do in G.4: the surface mesher's own degeneracies under a
  steeply graded sizing field, which is what now limits an adaptive
  cycle. Under a 3x local size reduction it emits collinear nodes and a
  zero-area triangle, and no amount of flipping or smoothing repairs a
  degenerate input. Ruppert refinement with proper encroachment handling
  (noted as absent in G.2) is the fix, and it is a feature rather than a
  repair.

### G.5: the payoff, and the reason it needs a net

`MakeSecondOrder` in `src/fem_mesh_curved.cpp`. A first-order tetrahedron
has flat faces, so a mesh of them approximates a curved boundary by
chords and the error in the boundary is the error in the answer however
fine the elements get. A second-order one carries a node at the middle of
each edge and its faces are quadratic -- but only if the mid-node is put
where the curve is rather than halfway between its ends, and only the
exact geometry knows where that is. **A pipeline that meshes a
tessellation cannot do this at all**: by the time it sees the model the
curve is already chords, and the middle of a chord is exactly where the
mid-node should not go.

The numbers, against volumes that can be written down:

| body | straight | curved | exact | error |
|---|---|---|---|---|
| box | 240.0000 | 240.0000 | 240 | nothing to curve, and nothing changed |
| cylinder | 74.2344 | 75.3968 | 75.3982 | 1.55% → 0.0019% |
| sphere | 107.4477 | 113.0661 | 113.0973 | 4.99% → 0.028% |

The box is the control and it matters as much as the other two: a planar
face has nothing for the geometry to say, so *no* mid-node may move, and
the volume must come back bit-for-bit unchanged. Getting a number that is
merely very close there would mean the projection was moving nodes it had
no business moving.

Three things make this correct rather than merely close.

**Only an edge of the boundary may be curved.** Two nodes lying on the
same CAD face does not make the edge between them an edge of that face --
a chord straight through the middle of a sphere has both ends on it --
and pulling that chord's midpoint out to the surface would turn an
element inside out for no reason at all. The edges genuinely on the
surface are exactly the edges of the boundary triangles, so that is what
is asked.

**An edge on a CAD edge is curved parametrically, not by searching.** The
mid-node is the point at the average of its ends' parameters, which needs
no projection and so cannot land on the wrong branch of anything. Where a
search is needed -- a mid-node inside a face -- the result is accepted
only if it lands within half the edge's own length of the straight
middle, which is far more than any real sagitta and far less than the far
side of a cylinder across its seam.

**And validity is asked of the map, not of the corners.** A quadratic
map can fold in the middle of an element whose four corners are all
exactly where they should be, so the Jacobian is checked at the corners,
the centroid and the six mid-edge points, and the fold shows up at the
mid-edge points first. Where an element is inside out its mid-nodes are
halved back towards the straight position and the sweep runs again --
they are shared, so fixing one element must not be allowed to have broken
another -- ending at the straight position, which is always valid because
the first-order mesh was checked. `CheckMesh` asks this too, so nothing
downstream can move a mid-side node and get away with it.

**The net is never needed in normal use, which is exactly why it is
tested on purpose.** The sizing field's curvature rule caps how far round
a curve one element may go, which caps the sagitta, which is the distance
the mid-node moves -- so on every body meshed at its default settings,
nothing is backed off. The test therefore relaxes that rule deliberately:
a sphere at more than half a radian per element backs off 18 nodes, stays
valid, and still recovers most of the volume (89.46 straight, 111.53
curved, 113.10 exact). A safety net that is never tested is a safety net
that does not work.

### G.6: a profile times a length

`MeshSweep` in `src/fem_mesh_sweep.cpp`. **Why bother, given a
tetrahedral mesher that works?** Tet4 locks in bending -- a beam with a
few of them through its thickness comes out far too stiff -- and it is
worse again under plasticity and near-incompressibility, where constant
strain over each element leaves the volume-preserving part of the
deformation nowhere to go. Hex8 has none of those problems, and a
prismatic part is exactly the sort that gets bent. The bodies that most
want hexes are the ones whose shape makes hexes easy, which is the whole
argument for a mesher that handles only some bodies.

A prismatic body is a profile times a length: mesh the profile with
quadrilaterals, copy it up the length in layers, join layer to layer.
Nothing has to be discovered -- the element count is quads times layers
and the boundary is known before it is built -- which is the difference
between this and the tetrahedral mesher, and why the boundary here is
*constructed* rather than found by looking for faces used once. Built
that way, every boundary quadrilateral knows which CAD face it came from,
which is what Part H.2 needs to put a pressure on a face.

**Quadrilaterals from triangles by subdivision, not by pairing.** The
obvious route is to mesh with triangles and glue adjacent pairs into
quads. It is obvious and it does not work well: the pairing is a matching
problem, some triangles always end up without a partner, and a mesh that
is mostly hexes with wedges scattered through it needs two element
libraries to solve and two sets of quality rules to judge. Splitting each
triangle into three quadrilaterals instead -- corner, edge midpoint,
centroid, other edge midpoint -- always works, for any triangulation of
any profile with any number of loops, and gives every element the same
type. It costs three times as many elements and each is better shaped
than the triangle it came from: an equilateral triangle yields three
quadrilaterals whose worst angle is sixty degrees. The profile is
smoothed once before it is swept, which is worth as many passes as there
are layers, since every layer inherits it.

| body | elements | volume | exactly | worst scaled Jacobian |
|---|---|---|---|---|
| box | 240 hexes | 240.0000 | 240 | 0.600 |
| cylinder | 1218 hexes | 73.8567 | 73.8567 | 0.459 |
| block with a hole through it | 3870 hexes | 228.2968 | 228.2968 | 0.498 |
| L-shaped block | 456 hexes | 192.0000 | 192.0000 | 0.600 |

The cylinder's and the drilled block's "exactly" are the *faceted*
volumes, and that is the honest comparison rather than a weaker one: the
mesh is a swept polygon, so a sweeper that returned the true 75.3982
would have moved its boundary off the profile it swept. The drilled block
measures 228.30 against a truly drilled 227.73 and a solid 256, which is
how one knows the hole is really there and really that size.

**Every candidate direction is tried, not the first that looks right.**
A drilled block has three pairs of parallel faces and only one of them is
the sweep. Committing to the first pair found takes the block's sides,
finds the hole is not parallel to them, and reports a perfectly sweepable
body as unsweepable.

**And the refusals are the point as much as the meshes are.** A blind
hole is the case that looks sweepable and is not: the top face has a hole
in it, the bottom does not, and the flat floor of the hole faces along
the only direction that could have worked. A mesher that swept it anyway
would fill the hole in silently, and a filled hole in a stress model is
not a mesh that is slightly wrong. So the side faces are checked against
the sweep direction and the body is refused by name.

### G.7: somebody else's opinion, and what it cost to ask

Every other test in this part checks the mesh against itself or against a
number that can be written down. Those catch a great deal and they cannot
catch one thing: a mesh that is valid, watertight, correctly sized and
simply much worse than it should be. Nothing internal says what "should"
is. So `mep-fem-mesh-compare-test` meshes the same bodies with Gmsh, at
the same requested size, and measures **both with the same code** --
this library's own metrics applied to the other mesher's elements.
Comparing Gmsh's quality report with ours would compare two definitions,
which is how one gets a comfortable answer and no information.

The geometry goes across as STEP, which makes it two tests at once: Gmsh
reads it through OpenCASCADE 7.8, so a mesh of the right volume says the
file described the right solid to a completely independent kernel. That
is a far stronger statement about Part F than reading our own file back
can ever be. Nix has a Gmsh built with OpenCASCADE, Netgen and TetGen, so
one binary gives both the Delaunay and the Netgen frontal opinion.

**And it immediately earned its keep.** The first run meshed zero
elements on every body, from files this library's own reader was
perfectly happy with, and Gmsh reported no error at all -- it said "Done
reading" and produced nothing. The export had a shape representation and
no product structure, and a receiver finds its roots by following
`SHAPE_DEFINITION_REPRESENTATION` back to a `PRODUCT_DEFINITION`. With no
product there are no roots, so the file reads cleanly and yields nothing.
Eight entities fixed it. **Nothing inside this repository would ever have
found it**, because our own reader does not look for roots, which is the
entire argument for the test existing.

The result, at the same requested element size:

| body | mesher | nodes | elements | volume | worst dihedral | slivers |
|---|---|---|---|---|---|---|
| box | mep | 106 | 277 | 240.0000 | 14.0 | 0 |
| | gmsh, delaunay | 307 | 974 | 240.0000 | 15.0 | 0 |
| | gmsh, netgen | 431 | 1744 | 240.0000 | 9.0 | 0 |
| cylinder | mep | 382 | 998 | 74.2344 | 13.9 | 0 |
| | gmsh, delaunay | 342 | 1157 | 74.2911 | 15.0 | 0 |
| | gmsh, netgen | 393 | 1483 | 74.2911 | 13.8 | 0 |
| sphere | mep | 171 | 571 | 107.4477 | 12.6 | 0 |
| | gmsh, delaunay | 196 | 627 | 109.0635 | 13.3 | 0 |
| | gmsh, netgen | 176 | 469 | 109.0635 | 12.2 | 0 |
| L-shaped | mep | 158 | 444 | 192.0000 | 15.3 | 0 |
| | gmsh, delaunay | 306 | 920 | 192.0000 | 14.5 | 0 |
| | gmsh, netgen | 324 | 1054 | 192.0000 | 11.7 | 0 |

Read across rather than down. The worst dihedral angle is within a degree
or two of Gmsh's Delaunay everywhere and better than its Netgen frontal
on the box and the L; the element counts are lower, sometimes by three
times, for the same requested size; and the volumes agree to the faceting
on the curved bodies and exactly on the flat ones -- including the L,
which is a boolean result, so Gmsh meshing 192.0000 of it says Part C's
subtraction and Part F's export are both right.

**The second-order mesh beats every first-order one and is checked
against them.** The sphere: 107.45 from us, 109.06 from Gmsh either way,
113.0661 from the same mesh of ours with its mid-side nodes put on the
exact surface, against an exact 113.0973. That is Part G.5's claim tested
against somebody else's mesh rather than only against arithmetic.

**Gmsh's meshes have to pass our checker**, and that check runs in the
strict direction: they are known-good meshes from a mesher twenty years
older than this one, so a rejection is the checker's fault. It is the
only test here that can catch a checker which is wrong by being too
strict -- every other one would simply pass.

---

## Part H — FEM core

`src/fem_elem.{h,cpp}`, `src/fem_model.{h,cpp}`, `src/fem_assemble.cpp`,
`src/fem_static.cpp` — all compiled into both `mep` and `mep-fem`.

- [x] **H.1 Element library.** `src/fem_elem.{h,cpp}`, covered by
  `mep-fem-elem-test` (12460 checks). Shape functions, derivatives,
  Jacobians and quadrature for all eleven: Tri3/6, Quad4/8, Tet4/10,
  Hex8, Hex20, Wedge6/15, Pyr5, with B-bar for the near-incompressible
  case. Every one passes its own patch test. Described below.
  - [ ] Incompatible modes or an assumed strain field, for the shear
    locking B-bar does not touch — a trilinear hexahedron in bending is
    still too stiff, and the note in `ElementOptions` says so.
  - [x] `fem_solve.cpp`'s own copy of Hex8 is gone; it calls this
    library's `ShapeFunctions` and `ElementStiffness` (done in H.3).
- [x] **H.2 The analysis model, bound to topology.** `src/fem_study.{h,cpp}`,
  covered by `mep-fem-study-test` (108 checks). Isotropic,
  temperature-dependent and orthotropic materials; supports and loads
  attached to **named** CAD faces and edges through B.4, resolved against
  whatever the model has been rebuilt into; consistent load vectors from
  pressure, traction, a total force and gravity. The same study binds
  unchanged to a block made taller, made wider, and remeshed finer.
  Described below.
  - [ ] Vertex targets. B.4 captures faces and edges and has no
    `CaptureVertexName`, so there is nothing to resolve one against yet.
  - [ ] A line load on an edge, which wants a one-dimensional element to
    integrate over. Refused by name rather than approximated.
  - [ ] Several sections at once. A `VolumeMesh` does not record which
    body each element came from, so a study with more than one material
    warns and uses the first. The gap is the mesher's, not this part's.
- [x] **H.3 Assembly.** `src/fem_assemble.{h,cpp}`, covered by
  `mep-fem-assemble-test` (17453 checks). Symbolic pattern from
  connectivity, then numeric fill into it; single-point constraints by
  elimination, multi-point ones and rigid links by Lagrange multipliers or
  penalty. `fem_solve.cpp`'s duplicate Hex8 is gone. Described below.
  - [ ] `fem::Model` still stands between this and `fem_solve.cpp`: its
    `Element` carries eight node indices, so the Part 0.6 solve path
    cannot yet take a tetrahedral mesh even though the assembly can.
    Replacing it with `AnalysisModel` is H.6's work, with the stress
    recovery that goes with it.
  - [ ] Rigid links are translation-only. A true rigid body needs
    rotational degrees of freedom at the controlling node, which a
    displacement-only solver does not have; the header says so rather than
    implying otherwise.
- [x] **H.4 Direct solution.** Sparse LDLᵀ with real AMD, now the default
  ordering, in `src/num_sparse.{h,cpp}` and covered by
  `mep-num-sparse-test` (16522 checks). A finished factor can be spilled
  to a file and solved from it. Described below.
  - [ ] Out-of-core *factorization*, which is a different factorization
    rather than an option on this one: the up-looking algorithm reaches
    back into arbitrary earlier columns, so a left-looking or multifrontal
    scheme is what makes the peak fit. What is here helps the case where a
    factor is computed once and solved against many times.
  - [ ] Choosing to spill automatically. `SpillTo` is explicit, because a
    numerics library inventing a directory policy is worse than a caller
    deciding.
- [x] **H.5 Iterative solution.** PCG with IC(0) came from Part 0.3;
  smoothed-aggregation algebraic multigrid is new, in
  `src/num_sparse.{h,cpp}`, covered by `mep-num-sparse-test` and by the
  elasticity comparison in `mep-fem-assemble-test`. Described below.
  - [ ] A W-cycle, and Chebyshev or Gauss–Seidel smoothing. Damped Jacobi
    is chosen for being symmetric, which is what lets the cycle be used
    with conjugate gradients at all; a symmetrised Gauss–Seidel would
    smooth better for the same reason it costs more.
  - [ ] Deciding between direct and iterative automatically. Both are
    exposed and the caller chooses; the crossover depends on memory,
    right-hand-side count and conditioning, and guessing it silently
    would be worse than asking.
- [x] **H.6 Linear static, verified.** `src/fem_static.{h,cpp}`, covered by
  `mep-fem-static-test` and by `mep-fem-static-compare-test` (`just
  test-fem-compare`). Displacement, stress at element centres and averaged
  to nodes, reactions, strain energy, on any element the library has and
  either solver. Verified by every element's patch test (H.1), by a
  manufactured-solution convergence study, by a closed form on curved
  meshed geometry, and against CalculiX on identical meshes. Described
  below.
  - [ ] The NAFEMS LE-series benchmarks. Their geometries are elliptic
    plates and thick annuli, and the two defects found while trying —
    Part G.2 not meshing a planar face with a hole in it, and Part C.4
    returning an open shell for an annulus intersected with a box — block
    the natural route to them. They are geometry work rather than solver
    work and they belong after those two are fixed.
  - [x] Stress recovery arrived with J.1 -- and by superconvergent patch
    recovery rather than the extrapolation named here, because
    extrapolation turned out to be worth nothing (see J.1's note).
  - [ ] `fem::Model`, `fem_solve.cpp` and the `mep-fem` process are now
    **unused**: nothing in the editor, in Lua or in the justfile spawns
    `mep-fem`, and `main.cpp` and `editor.cpp` never mention `fem::Model`.
    Part K's in-process session and `mep --cad-fem` do that job on
    `AnalysisModel`. What is left is a decision rather than work: retire
    the Part 0.5/0.6 pair, or give `mep-fem` the new model. Recorded
    here because "still the path the editor uses", which this said until
    the Part K sweep, had not been true for some time.

### H.1: eleven elements and the identities that catch them

`fem_elem.{h,cpp}`. Almost every mistake in an element -- a sign in a
derivative, a node in the wrong place, a quadrature weight that does not
sum to the reference volume -- produces a solver that runs, converges,
and is wrong by ten or twenty percent. None of it shows up as a crash. So
each element is held to properties that follow from what an element *is*,
none of which needs the right answer to any problem: the shape functions
sum to one, each is one at its own node and zero at the others, their
derivatives sum to zero, the isoparametric map reproduces a linear field
exactly, the quadrature weights sum to the reference volume and the rule
integrates to its stated degree, the stiffness matrix is symmetric, and
it has exactly six zero eigenvalues in three dimensions and no more.

**The rank check found the bug, and it is worth recording exactly how it
hid.** `ElementJacobian` used the inverse Jacobian transposed. The
identity `dN/dx_j = sum_i dN/dxi_i * dxi_i/dx_j` needs `inverse[j][i]`,
because the matrix is built with the reference index first; it was
written `inverse[i][j]`. Every element still passed partition of unity,
still passed the Kronecker delta, still produced a symmetric positive
stiffness matrix, and still carried no force under a rigid
*translation* -- because a translation only needs the derivatives to sum
to zero, which any linear combination of them does. What broke was rigid
*rotation*, which stopped being strain-free, and the elements quietly
lost three of their six zero eigenvalues. A solver built on it would have
run and been wrong.

Two smaller ones, both in the pyramid, which is the only element here
whose shape functions are not polynomials: the `zeta` factor was missing
from the collapsed term, so the functions were not one at their own
nodes; and its quadrature needs two more degrees than it is asked for,
because the reference pyramid's own Jacobian carries a factor of
`(1-zeta)` squared as the base shrinks to the apex. A one-point rule did
not even get the reference volume right.

**The patch test is the one an element exists to pass.** A patch of
elements, distorted so nothing holds by symmetry, with a linear
displacement imposed on every boundary node and the interior left free.
If the element can represent constant strain the interior must come out
*exactly* on that field -- not nearly, exactly -- because the exact
solution is in the space the element spans and a Galerkin method returns
the exact solution when it can. One construction serves every
three-dimensional element, since every one of them tiles a cube: eight
hexahedra with the middle node moved, then each cube carved into
tetrahedra, wedges or pyramids. All eleven come out at 1e-18 against a
field of size 1e-4.

**And B-bar, measured rather than asserted.** As Poisson's ratio
approaches a half, each quadrature point imposes its own
incompressibility constraint, and a mesh of low-order elements has more
of them than it has degrees of freedom to satisfy them with. A block
sheared sideways, reported against the shear modulus it should follow, so
that a correct element reads 1.00:

| elements | full integration | B-bar |
|---|---|---|
| 8 | 1.200 | 1.234 |
| 27 | 1.316 | 1.109 |
| 64 | 1.371 | 1.128 |
| 125 | 1.402 | 1.112 |

**It gets worse under refinement, which is the whole signature** --
refining adds constraints faster than it adds freedom, so the answer
walks away from the right one rather than towards it. That is the second
of this plan's diagnostic patterns, and it is why the test asserts the
*trend* rather than a threshold on one mesh: a single number here would
have been a guess, and the first guess made was wrong.

B-bar is flat in the mesh size, because the constraint is imposed once
per element rather than once per quadrature point. It does not touch
shear locking, which is a different fault with a similar name, and the
note in `ElementOptions` says so rather than leaving a reader to assume
otherwise.

### H.2: attached to the geometry, not to the node numbers

`fem_study.{h,cpp}`. **This is the feature the whole coupling exists
for.** Every mesh-file FEM tool attaches its boundary conditions to node
numbers, because a mesh file is all it has. That works exactly once:
change a dimension, remesh, and the node numbers mean something else, so
every constraint and every load has to be picked again by hand. It is the
largest ongoing cost of using such a tool and it is entirely an artefact
of where the conditions are attached.

Here a condition is attached to a *named* face or edge, in the Part B.4
sense: a name that records what generated the entity and what it is next
to, not where it happens to be. One study, captured once from a 10x6x4
block and never touched again:

| the model it was bound to | nodes | elements | held | top area | load |
|---|---|---|---|---|---|
| 10 x 6 x 4, as captured | 114 | 333 | 37 | 60.000 | 1.2e8 N |
| 10 x 6 x 12, taller | 284 | 1002 | 37 | 60.000 | 1.2e8 N |
| 25 x 6 x 4, wider | 227 | 683 | 73 | 150.000 | 3.0e8 N |
| 10 x 6 x 4, finer mesh | 385 | 1443 | 77 | 60.000 | 1.2e8 N |

Every row is a different mesh with different node numbers and a model
whose face ids are different, and in every one the pressure landed on the
top face and the bottom face was held. The wider block's load follows its
geometry, which is the point: the study says "the pressure on the top
face", so a bigger top face carries more load.

**And a name that cannot be honoured is reported, not guessed at.** A load
moved quietly onto the wrong face is worse than a study that will not
run: the answer looks plausible and nothing about it says which face it
was computed on. `BindStudy` fails by default and names what it could not
find; `allow_unresolved` turns that into a warning for an interactive
caller that wants to show what is still attached while the user fixes the
rest. A study with nothing holding it still is refused whatever its loads
say, because it has no unique solution.

**A trap worth recording, because nothing failed when it was hit.**
`ResolveFace` builds each candidate's name using the candidate face's own
`name` field as its role, so a target captured with any other role string
can never match on that signal -- and the role is a fifth of the whole
score. A box's six faces are alike in every other respect but their
normal and their position, so losing the role took the top face from
resolving to scoring 0.785 against a runner-up's 0.676: inside the
dominance margin, so reported as ambiguous. The study bound to the
original block and refused the same block made taller. Nothing was wrong
except a string. `FaceTarget` and `EdgeTarget` now capture it correctly,
which is why they exist rather than callers passing a role of their own.

**Loads are consistent, not divided equally.** A face's total force split
evenly between its nodes is wrong wherever the mesh is graded, and wrong
in a particular way on a second-order mesh: the correct share for a corner
node of a quadratic triangle is *negative*. So a pressure, a traction and
a total force all go through the same surface integral of the shape
functions, and gravity through the volume integral. Checked against what
is known rather than against itself: a uniform pressure's nodal forces
sum to pressure times area along the inward normal, **and their moment
about the origin matches the moment of the distributed pressure**, which
the sum alone cannot tell -- forces that summed right and were
distributed wrong would pass one and fail the other. Gravity sums to
density times volume times g, which is the mesher's volume claim checked
again from a third direction.

**The orthotropic matrix is built as a compliance and inverted**, not
written out as a stiffness. The nine constants a person has *are*
compliances -- a modulus is a strain per stress -- so every entry of the
compliance matrix is one input, while the stiffness in terms of the same
nine is a page of algebra with nowhere to check itself. It also makes the
stability requirement checkable: an inconsistent set shows up as a matrix
that is not positive definite, and is refused with the reason. Set all
nine constants equal and the result matches the isotropic matrix to
0.00e+00 -- two completely different routes to the same numbers.

### H.3: the structure once, the numbers many times

`fem_assemble.{h,cpp}`. Where every non-zero of the global matrix lies
follows from connectivity alone: two degrees of freedom share an entry
exactly when some element holds them both. That does not change when a
material changes, when a load case changes, or between the iterations of
a nonlinear solve -- only the numbers in it do. So the pattern is built
once and filled repeatedly, which is the difference between a nonlinear
solve that rebuilds its matrix from millions of triplets every iteration
and one that writes into a structure it already has.

It also makes the fill checkable, which is the better half of the reason.
A pattern derived from connectivity and a matrix derived from element
stiffness matrices are two independent derivations of the same set of
positions, and the test compares them entry for entry in both
directions -- nothing the connectivity implies missing from the pattern,
and nothing in the pattern that the connectivity does not imply. Filling
the same pattern twice is bit-for-bit identical, and the assembled matrix
annihilates all six rigid-body modes, which is the global form of H.1's
rank check and the thing that catches a mis-scattered element matrix. A
wrongly indexed assembly is still symmetric, so symmetry alone would not.

**Three ways to apply a constraint, and the test holds them to each
other.** A single-point constraint is eliminated: exact, smaller system,
and impossible for a constraint coupling several freedoms. Those get a
Lagrange multiplier, which is exact and makes the matrix indefinite and
larger, or a penalty, which keeps it positive definite and the same size
and is approximate by however large the penalty is not. On a bar whose far
end is required to stay flat:

| | mean extension | spread across the end |
|---|---|---|
| unconstrained | 1.001333e-05 | 2.74e-06 |
| Lagrange multipliers | 9.523810e-06 | 1.36e-20 |
| penalty | 9.523803e-06 | 3.26e-15 |

The unconstrained row is there because without it the rest would pass on
a constraint that did nothing: an end pulled by nodal forces really does
bulge, by a quarter of the mean. Lagrange satisfies the constraint to
round-off, penalty to a part in 1e9, and they agree on the answer -- which
is what says neither is merely self-consistent.

**The saddle point had to be scaled, and the failure looked like
something else entirely.** A constraint row's coefficients are of order
one while the stiffness is of order 1e11, so once the stiffness block is
eliminated the multiplier's own pivot is the Schur complement `-C K^-1
C^T`, of order 1e-11. A factorization that rejects a pivot small relative
to the *largest* diagonal then refuses a perfectly well-posed system, and
says "an unrestrained rigid-body mode is by far the usual cause" -- which
it is, and it was not this time. Multiplying each constraint equation
through by the largest diagonal changes nothing mathematically and brings
the pivot to the same order as the rest.

**And the bar test found a load that was not what it looked like.** With
the far end's force divided equally between its nodes the extension came
out five percent soft, because equal shares are a *non-uniform* traction:
a corner node covers a quarter of the area an interior one does. For a
structured grid the consistent share is exactly the tributary area, and
with that the extension is `PL/AE` to the last digit. It is Part H.2's
argument about consistent loads arriving from the other direction, in a
test that was not looking for it.

**`fem_solve.cpp`'s duplicate Hex8 is gone.** Part 0.6 wrote the shape
functions, the Jacobian and the strain-displacement matrix into that file,
which was right for one element and became a second place for a sign to be
wrong the moment H.1 existed. The two were compared on a *distorted*
element first -- an undistorted one agrees under almost any mistake -- and
agreed to 3e-5 in 1e11, which is round-off. Then the duplicate was
deleted and the 1533 checks of `mep-fem-test` still pass.

### H.4: the ordering was the fallback, and the fallback is gone

`ApproximateMinimumDegree` in `num_sparse.cpp`. Minimum degree works by
forming, explicitly, the clique that elimination creates: eliminate a
node and every pair of its neighbours gains an edge. That is exactly
right and it is why it is quadratic -- a node of degree *d* costs *d²*
insertions, and in three dimensions *d* grows as elimination proceeds, so
the graph being walked becomes far denser than the matrix it came from.
Past a few tens of thousands of unknowns the ordering cost more than the
factorization it exists to speed up, and `ComputeOrdering` quietly
returned Cuthill-McKee instead -- a caller asking for one ordering and
getting another.

AMD never forms the clique. An eliminated node becomes an *element*, and
a live variable records which elements it touches rather than which
variables they imply, so a clique costs its size rather than its size
squared. Three further things matter: **approximate degrees**, computable
in one pass over the element lists and exact whenever a variable touches
at most two elements, which most do; **supervariables**, which on a
structural mesh is not a small saving, because the three degrees of
freedom of a node have identical patterns and the graph collapses
threefold before anything else happens; and **mass elimination with
element absorption**, which take the free cases out immediately.

On a three-dimensional Laplacian:

| unknowns | AMD fill | AMD time | exact min-degree fill | its time | RCM fill |
|---|---|---|---|---|---|
| 512 | 10 899 | 0.001s | 11 756 | 0.005s | 19 054 |
| 2 744 | 145 278 | 0.007s | 161 599 | 0.291s | 304 941 |
| 8 000 | 812 003 | 0.060s | 967 669 | 5.219s | 1 796 849 |

Eighty-seven times faster at eight thousand unknowns, and **sixteen
percent less fill than the exact algorithm it approximates**, which is
worth being clear about rather than glossing: the approximation is of the
*degree*, which is a heuristic choice of pivot, not of the
factorization. Exact minimum degree breaks ties by whichever node its
linear scan reaches first; AMD's supervariables and mass elimination
break them in a way that happens to be better here. Neither is "the
right" ordering -- finding that is NP-hard -- and the exact one is kept
because it is the definition the approximation is measured against.

**Out of core, and what it is not.** A finished factor can be written to
a file, released from memory and solved from disk: the two triangular
solves walk the columns forwards and then backwards, so it is sequential
reading in both directions rather than random access, which is what makes
it worth doing. The test checks that the memory is genuinely released --
a cleared vector still holds its allocation, so it asks for the number --
and that the answers are **the same bits**, not merely close, because
streaming changes nothing about the arithmetic and agreement to round-off
would mean something had changed that should not have. A spilled factor
whose file has gone is a failed solve rather than a solve on whatever was
in the buffer, which is the failure mode worth checking because the
object still looks perfectly factorized.

The *factorization* still needs the whole factor in memory. Making that
out-of-core means a left-looking or multifrontal scheme, where a column
is finished and never revisited, and that is a different factorization
rather than a flag on this one.

### H.5: the preconditioner that stops caring how fine the mesh is

Jacobi and incomplete Cholesky are *local*: they damp error that varies
quickly from one unknown to the next and do nothing to error that varies
slowly across the whole model. So their iteration counts grow as the mesh
is refined, on top of the growth from having more unknowns. Multigrid
represents the slowly varying error on a coarser problem, where it is no
longer slowly varying, and recurses.

On a three-dimensional Laplacian, conjugate gradients to a relative
residual of 1e-10:

| grid | unknowns | Jacobi | IC(0) | multigrid |
|---|---|---|---|---|
| 8 | 512 | 20 | 13 | 11 |
| 12 | 1 728 | 32 | 19 | 13 |
| 16 | 4 096 | 44 | 23 | 14 |
| 20 | 8 000 | 56 | 29 | 13 |

Read down the columns. The point is not that 13 beats 29; it is that 13
is still 13 while 29 was 13 four rows earlier. The test asserts the
trends rather than any single number, because on one grid a
preconditioner can be made to look good by tuning.

**Three things had to be right, and two of them were wrong first.**

*Strength of connection* was measured against `sqrt(a_ii * a_jj)`, which
is the textbook form and works on the finest level, where the operator is
the one the discretisation produced. It fails on the coarse levels, where
the Galerkin product has spread each row over more columns and rescaled
it: measured that way the coarse rows had almost no strong connections,
so almost every node became an aggregate of one. The hierarchy went 4096,
514, 396 -- a level that costs work and buys nothing -- at an operator
complexity of 4.32. Against the row's own largest off-diagonal the test
is free of both scalings: 4096, 514, 26, at complexity 1.49.

*The near-null space* is what makes this work on elasticity at all, and
aggregation has to happen on **nodes** rather than on unknowns. Told
nothing, the aggregator works on the matrix graph, where a node's x, y
and z rows are three separate vertices that may land in three different
aggregates; the six rigid-body modes then describe nothing in particular
and the method degenerates to a slightly better Jacobi. And with several
candidates a coarse level carries that many columns per aggregate, so it
can be *larger* than the level above it -- six rigid modes over aggregates
of four rows is a hierarchy that grows instead of coarsening, level after
level, until a vector allocation throws. The guard has to be on the
column count, not the aggregate count. Along the way, Gram-Schmidt
annihilating a dependent candidate left an empty column in the
prolongator, which is a zero row and column of `P^T A P`, which is a
singular coarse operator -- the build then failed and the solver fell
back to Jacobi, which is a multigrid method that is not one.

On a cantilever, with the rigid modes supplied and block size three:

| elements | unknowns | IC(0) | multigrid |
|---|---|---|---|
| 128 | 600 | 28 | 24 |
| 432 | 1 764 | 41 | 31 |
| 1 024 | 3 888 | 53 | 32 |
| 2 000 | 7 260 | 65 | 35 |

Flat across the last two refinements where incomplete Cholesky is still
climbing. The comparison forces the same number of levels at every size,
because with only two levels the "coarse solve" is a direct factorization
of a quarter-sized problem -- the preconditioner is then very nearly
exact, the count flatters it, and it jumps when a third level appears,
which looks like a failure of mesh independence and is a change of
method.

**And the V-cycle has to be symmetric or conjugate gradients is not
valid.** Not approximately: the derivation assumes a symmetric positive
definite preconditioner, and an asymmetric one does not converge more
slowly, it converges to the wrong thing. That is why the smoother is
damped Jacobi rather than Gauss-Seidel and why there are the same number
of sweeps before and after the coarse solve, and it is checked --
`<Mu,v> - <u,Mv>` comes to 3e-16 against a scale of 238 -- rather than
argued.

Two sweeps rather than one, measured rather than chosen: on the
cantilever at four thousand unknowns one took 41 iterations and two took
32, for one extra damped-Jacobi pass in each direction. Three took 28,
which no longer pays for itself.

### H.6: three verifications, because a residual is not one

A linear solver can satisfy `||Ku - f||` to round-off while solving the
wrong problem, because the residual only ever sees the matrix it was
handed: a wrong element, a wrong load vector and a wrong constraint all
produce a system that is then solved perfectly. So there are three checks
that the residual cannot make, and the patch tests of H.1 underneath them.

**A manufactured solution, and the rate.** Choose a displacement field,
work out analytically what body force it is the answer to, apply that
force, and compare against the field. The choice matters: a
divergence-free field makes the awkward `grad(div u)` term of the Navier
equation vanish identically, so taking the curl of `(0, 0, sin(pi x)
sin(pi y) sin(pi z))` gives a field whose body force is `3 pi^2 mu u` --
one line, with no chance of the "exact" solution being exact for a
different problem than the one solved.

| element | h | L2 error | order |
|---|---|---|---|
| Hex8 | 0.2500 | 2.1145e-01 | 1.837 |
| | 0.1250 | 5.3928e-02 | 1.971 |
| | 0.0833 | 2.4043e-02 | **1.992** |
| Tet4 | 0.2500 | 3.5596e-01 | 1.289 |
| | 0.1250 | 1.1324e-01 | 1.652 |
| | 0.0833 | 5.3440e-02 | **1.852** |
| Tet10 | 0.5000 | 2.1098e-01 | 1.861 |
| | 0.2500 | 2.7095e-02 | 2.961 |
| | 0.1667 | 7.3967e-03 | **3.202** |

Two against an expected two, and three against an expected three. The
rate is judged on the finest pair and on the trend, not on every pair: it
is asymptotic, and on the coarsest mesh a linear tetrahedron manages 1.29,
which is the mesh being coarse rather than the element being wrong. A
wrong element fails both tests.

**A closed form on curved geometry.** A solid cylinder under uniform
external pressure, held axially at both ends so plane strain applies:
`sigma_r = sigma_theta = -p` everywhere and `u_r = -r p (1+v)(1-2v)/E`.
The stress state is *uniform*, so the patch test guarantees a correct
element reproduces it exactly and any error is the faceting of the
boundary the pressure was applied to. Radial displacement came to
-4.952371e-05 against an exact -4.952381e-05; the in-plane stress averaged
-1.0000e7 against an exact -1.0000e7 with a spread of one part in a
thousand across the mesh; the axial stress -6.0002e6 against the
-6.0000e6 plane strain gives. That exercises what the manufactured
solution does not: the mesher, Part H.2's pressure integral over curved
facets, and stress recovery.

**And somebody else's solver, on the identical mesh.** The two above say
this solver converges to the right answer at the right rate, which is the
strongest thing a code can say about itself. Neither says anything about
the conventions it shares with the rest of the world -- a node ordering, a
Voigt ordering, a sign on a prescribed displacement. Those are all
self-consistent here, and a self-consistent convention nobody else uses
is a code that gives right answers to models nobody else can read. So the
same mesh goes to CalculiX as Abaqus `.inp`:

| element | nodes | elements | mep tip | CalculiX tip | worst node |
|---|---|---|---|---|---|
| Hex8 | 135 | 64 | 2.279892e-04 | 2.279892e-04 | 2.2e-07 relative |
| Tet4 | 135 | 384 | 1.753756e-04 | 1.753756e-04 | 2.8e-07 relative |
| Tet10 | 135 | 48 | 2.504636e-04 | 2.504636e-04 | 2.0e-07 relative |

Agreement to two parts in ten million, which is the limit of what
CalculiX prints. The element behaviour is worth reading as well: the same
cantilever deflects 1.75e-4 with linear tetrahedra and 2.50e-4 with
quadratic ones, which is H.1's remark about Tet4 locking in bending,
measured on a real problem by two codes that agree about it.

**A defect of its own, which the manufactured solution found.** The
equilibrium residual was reported relative to the *sum* of the applied
forces, and that is wrong for any self-equilibrated load: a body force
pushing one way over half the model and the other way over the rest sums
to nothing, so dividing by it turns a residual at round-off into a number
in the hundreds. The manufactured body force is exactly that case, and
every mesh of a correct solve was reported as broken. The scale is the
total *magnitude* of the forces involved, applied and reacted.

---

## Part I — The rest of the physics

- [x] **I.1 Steady-state heat conduction.** `src/fem_thermal.{h,cpp}`,
  covered by `mep-fem-thermal-test` (28 checks). Conduction with
  fixed-temperature, flux, convection and radiation conditions, plus
  temperature-dependent conductivity, on any element the library has.
  Described below.
  - [ ] The consistent tangent for a temperature-dependent conductivity,
    which is *not symmetric* and so needs a non-symmetric solve. GMRES is
    there for the iterative case and there is no LU for the direct one.
    Until then that one nonlinearity is iterated as a fixed point and
    converges linearly; radiation and convection have their consistent
    tangents and converge quadratically.
  - [ ] A volumetric source as a field rather than one constant per
    element. Second-order accurate as it stands, which matches the
    element, but a sharp source wants better.
- [x] **I.2 Transient heat.** Generalised-θ in `src/fem_thermal.{h,cpp}`,
  covered by `mep-fem-thermal-test` (53 checks). Backward Euler,
  Crank–Nicolson and forward Euler, a consistent heat-capacity matrix,
  Newton within each step for the nonlinear conditions, and adaptive
  stepping by step doubling. Described below.
  - [ ] A lumped capacity matrix as an option. The consistent one is
    right for accuracy and the lumped one is what makes an explicit step
    cheap, and the explicit path is the only caller that wants it.
  - [ ] Second-order backward difference (BDF2), which is second order
    *and* monotone where Crank–Nicolson is only the former.
- [x] **I.3 Thermal–structural coupling.** Sequential, with the field
  mapping in `src/fem_map.{h,cpp}` and the thermal load in
  `src/fem_static.cpp`, covered by `mep-fem-couple-test` (26 checks). A
  temperature per node reaches both the constitutive matrix and the thermal
  strain, and the two meshes are deliberately different. Described below.
  - [ ] Two-way coupling, where the deformation changes the thermal
    problem — a contact gap that opens, or a radiating surface that moves.
    Sequential is one-way by construction.
  - [ ] Conservative mapping. The interpolation is *consistent* — it
    reproduces a linear field exactly — but does not conserve the integral
    of the field, which matters for a flux and not for a temperature.
- [x] **I.4 Modal analysis.** `src/fem_modal.{h,cpp}`, covered by
  `mep-fem-modal-test` (58 checks). Consistent and lumped mass, subspace
  iteration with shift-and-invert, mass-orthonormal shapes, and the Sturm
  sequence check. Described below.
  - [ ] Block Lanczos. Subspace iteration is what is written; asking for
    Lanczos is *refused with a message* rather than silently given
    subspace iteration, because a caller who asked for it because the
    other was too slow would otherwise get the same run time and conclude
    the method makes no difference.
  - [ ] The NAFEMS FV free-vibration series, which needs its geometries —
    the same blocker the LE series has, recorded under H.6.
  - [ ] Multi-point constraints, which need the mass projected onto the
    constraint null space. Refused by name rather than approximated.
- [x] **I.5 Linear buckling.** Stress stiffness and the buckling solve in
  `src/fem_modal.{h,cpp}`, covered by `mep-fem-modal-test` (108 checks).
  The stress stiffness is integrated from the static stress *field* at each
  quadrature point, and the eigenproblem reuses I.4's subspace iteration
  posed the other way round. Described below.
  - [ ] Multi-point constraints, which need the stress stiffness projected
    onto the constraint null space; refused by name.
  - [ ] Buckling from a *nonlinear* pre-stress state, which is what a
    structure that deflects appreciably before it buckles actually needs.
    That is Part I.6's territory.
- [x] **I.6 Geometric nonlinearity.** Total-Lagrangian formulation with
  Green–Lagrange strain and the second Piola–Kirchhoff stress,
  Newton–Raphson with an energy line search, and arc-length (Riks)
  continuation, in `src/fem_nonlinear.{h,cpp}`. Covered by
  `mep-fem-nonlinear-test` (265 checks with I.7 and I.8). Described below.
  - [ ] Follower loads — pressure that turns with the surface it acts on.
    Every load here keeps its original direction, which is right for
    gravity and wrong for pressure.
  - [ ] Corotational shells and beams; only solid elements are formulated.
  - [ ] Bifurcation detection along the path: arc length walks round a
    limit point but does not notice a branch.
- [x] **I.7 Material nonlinearity.** J2 plasticity with isotropic and
  kinematic hardening and radial-return integration, in
  `src/fem_plastic.{h,cpp}`, with the consistent algorithmic tangent
  obtained by differencing the return map itself.
  - [ ] Hyperelasticity (Neo-Hookean, Mooney–Rivlin), which was named
    here and is not written.
  - [ ] The plastic state is not yet carried by the global solve: the
    return map and its tangent are verified as a constitutive routine at
    a point, not driven through a structural Newton loop.
  - [ ] Creep, viscoplasticity, damage.
- [x] **I.8 Contact.** Node-to-surface penalty against a rigid plane, in
  `src/fem_contact.{h,cpp}`, with the active set reported from every
  solve. The penetration follows the series-spring closed form to 0.34%,
  which is the check that the constraint is enforced rather than
  approximated by luck.
  - [ ] Segment-to-segment mortar with augmented Lagrangian, which is
    what makes a contact *pressure* come out smooth and removes the
    penetration the penalty leaves.
  - [ ] Coulomb friction (and the unsymmetric tangent it brings).
  - [ ] Deformable-to-deformable and self-contact, which need the pairing
    searched each iteration rather than named up front. Depends on E.6
    assemblies for the multi-body case.

### I.1: the interesting part is the boundary conditions

A scalar field, so one unknown per node instead of three, and everything
Parts H.1 to H.5 built applies with the vector parts taken out: shape
functions, quadrature, a sparsity pattern, constraint elimination, both
solvers. Everything genuinely new is in the four boundary conditions, and
**two of them are nonlinear**, which is why this is the right first
physics after statics rather than a formality.

  * A fixed temperature is eliminated exactly as a prescribed
    displacement is.
  * A flux is a load, integrated over the face as a pressure is.
  * **Convection is not a load.** `h(T - T_ambient)` depends on the
    unknown, so part of it belongs on the left: it adds
    `h * integral(N N^T)` to the conductivity matrix and
    `h * T_ambient * integral(N)` to the load. Treating all of it as a
    load and iterating converges slowly or not at all.
  * **Radiation is nonlinear to the fourth power**, linearised about the
    current temperature with the tangent `4 e sigma T^3`.

Each is checked against an answer written down rather than computed, because
they fail differently: a flux integrated wrongly gives the right shape and
the wrong level; convection on the wrong side of the equation gives a
plausible field that does not satisfy its own boundary condition; and
radiation linearised wrongly converges to the right answer slowly enough
to look like it works.

| case | result | closed form |
|---|---|---|
| two fixed temperatures | worst node error 1.1e-13 K, flux 22500.0000 W/m² | 22500 exactly |
| flux in, convection out | worst node error 1.2e-11 K, one Newton sweep | `T_a + q/h + q(L-x)/k` |
| flux in, radiation out | face at 586.4978 K, worst error 6.8e-10 K, 8 sweeps | 586.4978 |
| manufactured source | order 1.889 at h = 1/24 | 2 |

The radiation case is worth singling out: the face temperature solves a
quartic, and the test finds it by **bisection**, which shares no code with
the solver's Newton iteration. Agreeing to 7e-10 is therefore a statement
about both. And the energy residual -- everything applied plus everything
reacted summing to zero -- is at round-off in every case, which is a
statement about the assembled system rather than about any one element.

**A finding, from writing the consistent tangent and watching it
diverge.** Differentiating the residual of a temperature-dependent
conductivity properly adds `dk/dT * N_b * (grad N_a . grad T)`, which
carries one index on a shape function and the other on a gradient, so it
is **not symmetric**. Handed to a factorization that assumes symmetry it
gave 6400 K on a slab running between 800 and 300. The tangent is right
and the solver is right and they do not fit: the fix is the non-symmetric
path, and until then that nonlinearity is a fixed-point iteration -- which
converges, linearly, in thirty-nine sweeps where a Newton step would take
a handful. The test asserts the thirty-nine, so that making it quadratic
later shows up as the improvement it is. It is the same mistake Part I.7
warns about for plasticity, met here first.

**And one where the solver was right and the test was wrong.** The test
expected a slab with falling conductivity to bow *above* the straight
line. It bows below: conductivity falling with temperature means the hot
end conducts badly, so the gradient is steep there and shallow at the cold
end, and the temperature drops quickly and then levels off. The solver
said 490.98 K against a linear 550; the expectation was simply
back-to-front.

### I.2: the step size is the whole subject

The step is `C (T' - T)/dt = theta S(T') + (1 - theta) S(T)`, where `S` is
the steady residual Part I.1 already assembles and `C` the heat capacity.
Sharing `S` between the two solvers is not tidiness: two copies of that
assembly would be two places for a boundary condition to be integrated
differently, and the difference would show up as a transient that settles
to the wrong steady state — the hardest kind of disagreement to find,
because each solver would be self-consistent.

**The three values of theta are three different methods, and the
differences matter more than the parameter suggests.** Backward Euler is
first order and unconditionally stable *and monotone*: it cannot
overshoot, whatever the step, which is why it is the default — a thermal
transient's first steps are always too large for the gradients in them.
Crank–Nicolson is second order and unconditionally stable without being
monotone: it oscillates around a sharp front rather than diverging from
it, which looks like a mesh problem and is not. Forward Euler is explicit
and only conditionally stable.

| what was measured | result |
|---|---|
| quenched slab against its Fourier series | worst node error 0.0391 K of a 200 K transient; energy residual 3.0e-14 |
| order in time, theta = 1 | 1.033 |
| order in time, theta = 0.5 | 2.000 |
| adaptive against 4000 fixed steps | 164 steps, 5 rejected, step grew from 0.00625 s to 10.9 s, agreed to 0.0300 K of a 25.1 K rise |
| explicit stability limit | 4.012 s, against `h²/(2α)` = 4.012 s |

**The order is measured against a reference on the same mesh, not against
the analytic answer**, and that is the only way it works. The spatial
discretisation has an error of its own which does not shrink when the step
does, so comparing with the exact solution measures the sum of the two and
reports an order of zero as soon as the spatial part dominates. A
reference taken with a very small step on the same mesh carries the same
spatial error, which therefore cancels.

**Adaptive by step doubling**: one step of `dt` against two of `dt/2`,
whose difference is proportional to the local error. It costs three solves
per accepted step and needs no second integrator and no embedded formula,
so what it estimates is the error of the method actually being used. The
step grew by a factor of seventeen hundred over the transient, which is
the shape every thermal problem has and the shape a fixed step handles
worst.

**The explicit limit came out at exactly `h²/(2α)`**, computed from the
assembled matrices by a Gershgorin bound rather than from the formula --
which is the point, since matching a formula one has substituted into
proves nothing. Asked for four times that step the solver refuses and says
why: an explicit method past its limit does not give a slightly worse
answer, it gives oscillations that double every step, and the result looks
like a modelling problem.

**And the energy check cried wolf until it was right.** Summing the steady
residual over every node gives the *applied* heat and nothing else,
because the conduction terms cancel internally — the shape function
derivatives sum to zero. The heat a *fixed* node supplies to hold itself
at temperature is not in that sum, and for a slab quenched through its
faces it is the entire energy flow. Left out, the check reported a
residual of 1.00 on a solve whose answer matched a Fourier series to four
hundredths of a kelvin. A check that cries wolf is worse than no check, so
it now carries both terms and reads 3e-14.

### I.3: the mapping is the deliverable, and the tests found three of my errors

Solving a thermal problem and then a structural one on the *same* mesh
needs no mapping at all, and that is the case that teaches nothing: every
later multiphysics step has the two fields on different meshes, because
the mesh a thermal gradient wants — elements through the thickness of a
wall — is not the mesh a stress concentration wants. So the mapping is
built and tested on meshes that deliberately differ, with the inverse
isoparametric map by Newton, a grid to find the candidate element, and a
tolerance in *reference* units so that it means the same thing on a
millimetre part and a metre one.

| what was measured | result |
|---|---|
| free block, uniform rise | corner moved exactly `alpha dT L`; worst von Mises **1.5e-6 Pa** against 9.45e8 restrained |
| fully restrained, uniform rise | **-9.450000e+08 Pa**, against `-E alpha dT/(1-2v)` exactly; largest displacement 9.5e-20 m |
| linear gradient, Hex8 | 2.78e7 → 1.67e7 → 9.02e6 Pa as the mesh halves twice |
| linear gradient, Hex20 | **7.9e-6 Pa** on the coarsest mesh of all |
| linear field mapped between meshes | exact to 7.1e-15 K |
| inverse isoparametric map | reference coordinate to 1.3e-16 |
| thermal solve mapped onto a different structural mesh, then held | mean axial stress -2.5200e+08 Pa against -2.5200e+08 |

**The free uniform case is the sharpest test in this part.** A uniform
temperature on an unconstrained body strains it and stresses it not at
all, so the thermal load and the stiffness must cancel *exactly*. An error
in the load's integration, in the constitutive matrix, or in the stress
recovery forgetting to subtract the thermal strain shows up as a large
stress in the right places, which is the most convincing kind of wrong
answer. It reads 1.5e-6 Pa against a scale of 9.45e8.

**And the tests were wrong three times before the solver was wrong once.**

*Coordinates compared with `==`.* Nodes are built as `size * i / n`, and
`0.3 * 4 / 4` is not bit-identical to `0.3`, so `p.x == size.x` was false
at the far face and the constraints meant to hold a fully restrained block
were never applied. It expanded almost freely and the stress came out at
38% of the closed form — a clean-looking factor that invites a search for
a missing `(1-2v)`.

*A three-point pin.* The textbook minimal restraint — one corner in three
directions, a second in two, a third in one — is badly conditioned: it
leaves modes that are *nearly* rigid, a rotation with just enough shear to
keep the pinned node still, which strains the body very little and so has
a tiny pivot. The factorization refused the matrix and blamed an
unrestrained rigid-body mode, which is the usual cause and was not this
one. Three symmetry planes remove the same six motions using whole faces.

*And a restraint the exact solution does not satisfy.* For a linear
temperature gradient the stress-free displacement is
`u_x = G(x) - g'(y^2+z^2)/2`, `u_y = g(x) y`, `u_z = g(x) z` — quadratic in
*every* direction, not just along the gradient. So `u_x` is not zero on
the whole `x = 0` face, only at the one point where `y` and `z` both
vanish, and holding the face fixes a body that wants to dish. The
symptom was diagnostic: the stress *grew* with refinement, 1.70e8 to
4.01e8, because a finer mesh resolves the dishing it was being forbidden
better. With `u_x` held at a single node the Hex8 stress converges away
and Hex20 gives zero outright, because that quadratic field is in its
space and not in Hex8's.

### I.4: the failure mode is silence

`K phi = lambda M phi`, built on two things Part H already had: the
assembled stiffness, and a factorization that reports how many negative
pivots it found.

**Shift and invert, because the wanted modes are the small ones.** A power
method converges to the *largest* eigenvalue, and the largest natural
frequency of a mesh is an artefact of the mesh rather than anything the
structure does. Replacing the operator by `(K - sigma M)^-1 M` turns the
eigenvalues nearest `sigma` into the largest, so the same iteration finds
the modes that matter and finds them fastest. It costs one factorization,
which is why a modal solver is built on a direct solver rather than an
iterative one. For a free structure the shift goes slightly *below* zero:
`K` is singular there, and a small negative shift makes it factorable
while leaving the rigid modes where they are.

| what was measured | result |
|---|---|
| unrestrained block | exactly **6** zero modes, then 4886.6 Hz |
| fixed-free bar, longitudinal, 8 / 16 / 32 elements | +0.16%, +0.04%, +0.01% against `c/4L` |
| its third mode, same meshes | +4.05%, +1.01%, +0.25% |
| lumped vs consistent mass | 645.4862 ≤ **646.5243 exact** ≤ 647.5633 Hz |
| Sturm count at every cut | agrees, 0 through 8 |
| `phi^T M phi` | 1 to 5.6e-16, 0 between modes to 1.0e-16 |

**The Sturm sequence, because the failure mode is silence.** An iterative
eigensolver returns the modes it converged to; if it missed one — and a
pair of close or repeated frequencies is exactly when it does — nothing
about the answer says so. The shapes are orthogonal, the frequencies are
real and ordered, and a mode is simply absent. By Sylvester's law of
inertia the number of negative pivots of `K - sigma M` is the number of
eigenvalues below `sigma`, and it comes from a factorization rather than
from the iteration, so comparing the two is the only check that catches
it. The test makes the comparison at *every* frequency found rather than
once at the top, which turns one check into eight and catches a solver
that found the right modes in the wrong order as well as one that lost
one.

**Lumped mass is not a cruder consistent mass, it errs the other way.**
Consistent over-estimates every frequency and lumped under-estimates them,
so the two together bracket the answer — 645.49 and 647.56 around an exact
646.52. That is a property of the formulation rather than of this problem,
so a solver that had it backwards would be caught here whatever it was
solving.

**And the shapes are orthonormalised in the *mass* inner product**, not
the Euclidean one. Shapes orthogonalised the easy way come out nearly
right and not orthogonal, and a forced response built on them is wrong in
a way that only shows up away from resonance.

### I.5: the same eigensolver, posed the other way round

The stress stiffness is the second-order part of the strain a
displacement formulation drops — what makes a compressed strut softer in
bending than an uncompressed one, and eventually not stiff at all. It is
symmetric and *indefinite*: compression makes it negative and tension
positive, which is the whole physics.

That indefiniteness decides how the eigenproblem is posed. Buckling asks
for `(K + lambda K_G) phi = 0`, and the obvious rearrangement
`K phi = -lambda K_G phi` puts an indefinite matrix where a subspace
iteration needs to orthogonalise. Swapping them — `-K_G phi = mu K phi`
with `lambda = 1/mu` — puts the positive definite matrix where the method
needs it, and the largest `mu` is the smallest load factor, which is the
one that matters.

| what was measured | result |
|---|---|
| pinned-pinned, 10 / 20 / 30 elements | −1.48%, −1.17%, **−0.99%** against `pi^2 EI/L^2` |
| clamped-pinned, same meshes | +1.59%, +0.05%, **−0.18%** against 2.0457 times it |
| second mode over first, pinned-pinned | 3.996 (Euler says 4) |
| second mode over first, clamped-pinned | 2.969 (that condition says 2.96) |
| first shape against a half sine | correlation **1.000000** |
| doubling the applied load | factor ratio **2.000000000** |
| the same column in tension | **−17.0251** against +17.0251 in compression |

Coming out slightly *below* Euler is right rather than tolerable: a
three-dimensional solid has shear flexibility that Euler–Bernoulli does
not, and shear flexibility lowers a buckling load.

**Three things the tests said before the closed form agreed, each a
different kind of error.**

*Shear locking, arriving from upstream.* A column slender enough for
Euler's formula to mean anything has elements twenty-five times longer
than they are thick, and Part H.1 measured what a trilinear hexahedron
does in bending. Here it gave a critical load a hundred and twenty times
too high which *fell* with refinement — 2.1e6, 5.6e5, 2.7e5 — which is
locking easing rather than a solution converging. Hex20 has the bending
mode in its space.

*A boundary condition that was not what it was called.* Fixing `u_y` and
`u_z` across the far face does not clamp it: rotation about `y` tilts the
face, which moves its nodes along `x`, and `x` is free there. So the
column was clamped-pinned while being described as pinned-pinned. The
solver said so twice over — 2.24 times Euler against the 2.046 that
condition predicts, and a mode ratio of 3.00 against its 2.96 rather than
pinned-pinned's 4.0 — which is why **both** conditions are now built and
checked: that tests whether the solver responds to the end condition,
rather than whether it lands near a number.

*And a constraint that changed the structure.* Holding `u_y` at every node
to force weak-axis bending looks harmless. It forbids the cross-section's
Poisson contraction, which makes the beam a strip in cylindrical bending
and stiffens it by exactly `1/(1 - v^2)` — 9.9% at Poisson 0.3. The load
came out 8.3% high and climbing towards it: a discrepancy that looks like
mesh convergence and is a different structure.

**And a negative load factor is not a mistake.** It means the structure
buckles when the load is *reversed*, at the same magnitude. Reporting only
positive factors would hide exactly that, which is a real question for
anything whose load can reverse, so they are returned and it is the
caller's business which matter.

---

### I.6–I.8: three ways a structure stops being linear

Three different nonlinearities, and what is worth recording is that each
one broke in its own characteristic way.

**I.6, geometry.** Total Lagrangian: strain measured against the original
shape, so the formulation is exact under arbitrarily large rotation
rather than merely tolerant of it, and that is the first test.

| what was measured | result |
|---|---|
| internal force after a rigid rotation of 1° / 30° / 90° / **180°** | 1.6e-16, 1.5e-16, 3.5e-16, **2.7e-16** of a unit strain |
| tangent against a finite difference of the force | **2.25e-12** relative |
| a small load, against the linear solver | tip 4.279311e-06 vs **4.279310e-06** m |
| Newton residuals on that step | 5.00e-01, 2.37e-03, **6.72e-13** |
| a cantilever bent a long way | tip drops 0.0519 m (linear says 0.0520) and **draws in 0.0024 m** |
| a shallow arch under load control | **gave up at factor 0.0000** |
| the same arch by arc length | reaches **0.4480**, the last quarter of the path raising it by 0.000000 |

The drawing-in is the point of the whole part: a linear solve says a
bent cantilever's tip moves straight down, and it does not, because the
arc length of the beam is conserved. Nothing in a linear formulation can
produce that number.

Residuals 5e-01, 2.4e-03, 6.7e-13 is quadratic convergence, and it is
also the test of the tangent — a tangent that is nearly right still
converges, just linearly, so the *rate* is the diagnostic and the value
is not.

*Green–Lagrange precision.* `E = (F^T F - I)/2` is correct algebra and
loses half the significant digits at small strain, because `F^T F` is
`1 + 2e` and subtracting the one throws away everything below `2e`
relative to it. Computing it from the displacement gradient instead,
`E = (H + H^T + H^T H)/2`, has no cancellation and took the rigid-rotation
residual from 1e-8 to 1e-16.

**I.7, material.** Radial return, and then the tangent, twice.

| what was measured | result |
|---|---|
| at half the yield strain | 1.2500e+08 Pa against `E*strain` |
| slope past yield | within **5.8e-08** of `EH/(E+H)` |
| at four times the yield strain | **2.570755e+08** Pa against the closed form |
| isotropic, pulled to 1.0028e+09 then pushed | yields again at **+1.0756e+08** |
| kinematic, the same history | yields again at **+1.2574e+08** (Bauschinger) |
| algorithmic tangent vs finite difference | out by **8.3e-10** |
| the continuum elastoplastic modulus, same test | out by **7.9e-02** |

The last two lines are why the consistent tangent is a named thing. Both
matrices are "the" elastoplastic tangent; they differ by 7.88%, and only
one of them is the derivative of the map the solver actually integrates.
Keeping the continuum one available to compare against is what makes
that number visible instead of a footnote.

*I derived the consistent tangent analytically twice and had it wrong
both times.* It is now obtained by differencing `ReturnMap` — twelve extra
evaluations from the same incoming state. That is slower and it cannot be
wrong in a way the algebra can, which for a routine whose errors show up
only as a degraded convergence *rate* is the better trade.

*And a test that was not testing what it said.* The first tangent check
sat on the corner where the increment is elastic-to-plastic, where the
map genuinely has no single derivative, so a disagreement there was
meaningless. Moved to a wholly plastic increment.

**I.8, contact.** Node-to-surface penalty against a rigid plane.

| what was measured | result |
|---|---|
| 16 bottom nodes after a 1e-04 m gap closes | all touching |
| sunk in, penalty scale 1 | 1.788e-05 m |
| stiffness ratio inferred from that | 2.356 |
| predicted at ten times the penalty | 2.443e-06 m |
| measured at ten times the penalty | **2.451e-06 m, out by 0.34%** |
| the same push with no plane | goes **straight through**, 1.600e-04 m |

*Ten times the penalty is not a tenth of the penetration*, and asking for
that is the loose version of this check. The penalty spring is in series
with the block's own stiffness, so `penetration = interference / (1 + r)`
with `r = k_contact / k_block`: measuring `r` from the first run makes
the second a prediction with nothing fitted. 7.29 looked like a sloppy
pass and was the right answer.

*The force and the tangent disagreed in sign,* and the failure is worth
the entry because it does not look like a sign error. The penalty energy
is `k g^2 / 2`, so the internal force is `k g n` — pointing *into* the
plane, since `g` is negative — and the stiffness is `k n(x)n`. I wrote
the force flipped and the stiffness not. Newton then computes its steps
from a matrix that is not the slope of what it is driving to zero: the
residual stalled at a fixed 3.9e-01 N, the increment halved six times,
and the reported penetration was 3e-12 m, i.e. the solver was sitting
right at the constraint and unable to say so. **A residual that stops
falling while the answer looks plausible is an inconsistent tangent, not
a hard problem.**

*And the convergence test had no scale.* A model driven entirely by
prescribed displacement has no applied load, so normalising the residual
by the load norm fell back to 1.0 and the tolerance became an absolute
1e-9 N, which nothing in newtons and metres satisfies. Normalising by the
internal force instead is worse and instructively so: with no applied
load the residual *is* the internal force on the free rows, so the ratio
is identically one, which it printed thirty times. The scale that always
exists is the out-of-balance the increment opens with.

## Part J — Post-processing

- [x] **J.1 Result fields and recovery.** `src/fem_result.{h,cpp}`, covered
  by `mep-fem-result-test` (20388 checks), and wired into `SolveStatic` so
  every static result now carries the recovered field, the unaveraged
  per-element values and the discontinuity map. Three recoveries:
  evaluation at the nodes, extrapolation from the integration points, and
  superconvergent patch recovery. Invariants (von Mises, Tresca,
  principals, max shear, hydrostatic, triaxiality), four failure criteria
  and nineteen named scalar fields that a contour, a probe and an export
  all read through one function. Described below.
  - [ ] Recovery of *strain* and of derived element quantities (energy
    density, plastic strain); only stress is recovered.
  - [ ] Patch recovery does not know about material interfaces: a patch
    that straddles two materials smooths across a discontinuity that is
    really there. Equilibrium-constrained recovery (REP) is the answer
    and is not written.
- [x] **J.2 Visualisation.** `src/fem_render.{h,cpp}`, covered by
  `mep-fem-render-test` (231 checks). One decomposition into linear
  tetrahedra, and the drawable surface, cut planes and isosurfaces are all
  built on it; contour colouring reads J.1's fields so a probe and a plot
  cannot disagree; deformed shape with an auto scale; clipping; arrow
  glyphs for displacement and reaction. Described below.
  - [ ] A Hex20's curved faces are drawn straight: splitting one properly
    needs face and body centres, which are not nodes, so it is decomposed
    on its eight corners. A Tet10 splits on its own mid-edge nodes and
    keeps its curvature.
  - [ ] Clipping drops whole triangles rather than cutting them, so the
    exposed edge is ragged where the mesh does not line up with the
    plane. The clean face is what a section is for, and the two are meant
    to be drawn together.
  - [ ] Nothing here is wired into the editor's panes yet; these are
    arrays a caller assembles into a `gfx::Mesh`.
- [x] **J.3 Animation.** `src/fem_animate.{h,cpp}`, with
  `SolveTransientHeat` gaining `sample_times`/`samples` so a transient can
  be recorded at asked-for times rather than at its own adaptive steps.
  Covered by `mep-fem-report-test` with J.4 (589 checks). Produces frames
  and nothing else; the keyframe track, camera and video encoder are the
  3D modeller's and stay that way.
  - [ ] Nothing drives `Model3DRenderAnimationToVideoFile` from a result
    yet: the frames exist, the glue does not.
  - [ ] Only the thermal transient records a history. A nonlinear or
    modal-superposition transient would want the same `sample_times`
    treatment.
- [x] **J.4 Probes and graphs.** `src/fem_probe.{h,cpp}`: point probes,
  path plots by arc length, time histories at fixed points, and CSV and
  org-table export at seventeen significant digits. The probe is
  Part I.3's field mapping with a different name for its target, which is
  the whole reason there is no second point locator.
  - [ ] Edge and face probes that integrate along or over an entity,
    rather than sampling at points on it.
  - [ ] Nothing opens the exported table in a buffer; it is returned as
    text.
- [x] **J.5 Adaptivity.** `src/fem_adapt.{h,cpp}`, covered by
  `mep-fem-adapt-test` (174 checks). Zienkiewicz–Zhu estimation in the
  energy norm from J.1's recovered field, a refinement plan expressed as
  sizing-field refinements (`SizingOptions::refinements`, new), and a
  loop that re-meshes the *CAD model* each cycle and reports its
  convergence. Described below.
  - [ ] **The loop is limited by mesh quality, not by the estimator.** A
    graded sizing field is where this Delaunay mesher produces slivers, so
    the loop stops after two or three cycles with a worst dihedral angle
    below its own threshold. It stops gracefully and says why — the last
    good cycle's answer is a real answer — but the payoff cannot be
    demonstrated until G.4 grades cleanly.
  - [ ] Refinement only. The sizing field takes the smaller of what it is
    told, so nothing here can coarsen an over-refined region.
  - [ ] No error estimate for the thermal, modal or nonlinear solves; the
    estimator is written for linear statics.

### J.1: where the stress is sampled decides how good it is

Three recoveries, and the useful result is how they compare rather than
that they exist.

| what was measured | result |
|---|---|
| patch test, linear displacement, distorted meshes, Tet4/Tet10/Hex8/Hex20 | nodal error and neighbour gap **< 1e-16** of the stress |
| invariants: three characteristic coefficients over 20000 random tensors | reproduced to **5e-16, 1.1e-15, 6.5e-16** relative |
| Tresca over von Mises across those tensors | stays in **[1.0027, 1.154701]**; the bound is [1, 2/sqrt3] |
| Mohr–Coulomb with one strength, where the state straddles zero | **is** Tresca, to 4.4e-16 |
| Hex8, rate at the worst node, naive -> patch | **0.965 -> 2.204** |
| Hex8, rate on the surface / inside | **1.078 -> 2.736** / 1.817 -> 1.717 |
| Hex20, rate at the worst node | **1.368 -> 2.638** |
| Hex20, rate on the surface / inside | **1.619 -> 3.020** / 0.954 -> 0.792 |
| uniform tension through `SolveStatic` | nodal error **6e-18** of the stress |

**Extrapolating from the integration points is worth exactly nothing,**
and this is the finding I would not have predicted. It came out *bit for
bit identical* to evaluating at the nodes, on every mesh, for both
element types. It is not a bug: on an affine map a trilinear hexahedron's
stress field is exactly trilinear, so fitting eight shape functions
through eight Gauss points is exact interpolation, and re-reading that
same polynomial at the nodes returns what evaluating there returns. The
two differ only where the fit cannot be exact — a distorted element,
whose map is rational, or an over-determined rule. **Superconvergence is
a property of where the element is sampled, and moving a polynomial's
argument does not create it.**

**The integration points are not the Barlow points**, which cost a
rewrite. A Hex8 is integrated at 2x2x2 because one point leaves it rank
deficient, but a trilinear element's strain is superconvergent at its
*centre*, and the Gauss points sit h/(2 sqrt 3) away where the error is
O(h) rather than O(h^2). Sampling where the integration already happens
is the obvious shortcut and throws away the whole order: it measured a
rate of 1.19 where the reduced rule gives 2.20. The sampling rule is now
the element's own order — one point for a Hex8 or Tet4, 2x2x2 for a
Hex20, four for a Tet10.

**And the entire gain is at the boundary, which is the opposite of what I
assumed.** I split the error into interior and surface expecting the
surface to be patch recovery's weak point, since those patches are
borrowed from inside and evaluated beyond their data. It is its strong
point. Plain averaging over the elements meeting at an *interior* node of
a regular mesh is already superconvergent — the errors of elements on
opposite sides are equal and opposite and cancel in the mean — so patch
recovery gains no order there and loses a constant factor. That
cancellation is one-sided at a boundary node, which is why averaging is
only first order there, why the worst node of a mesh is always on the
surface, and why SPR is worth having: it is a whole order better exactly
where every result is read.

A consequence worth stating plainly: **patch recovery is better
asymptotically, not uniformly.** On the coarsest meshes here it is
several times *worse* in every norm, and only overtakes by the second or
third refinement. A single mesh's number would have called it a
regression; the rate is what says otherwise.

**On a hex mesh every surface node borrows.** A Hex8 corner touches one
element and so has one Barlow sample against a linear fit's four terms; a
node on a flat face touches four elements whose centres are *coplanar*,
so the normal matrix is singular in the direction off that plane however
many samples there are — counting points would call that patch fine, and
only the factorisation knows. At four elements a side that is 98 of 125
nodes. Borrowing is the common path, not an edge case.

**Stepping down the degree must come after borrowing, not before.** My
first version tried a lower-order fit on the node's own patch first. A
Hex20 corner has eight samples against a quadratic's ten terms, so the
step-down *succeeds*, reports itself as a perfectly good fit, and quietly
makes the recovery linear all over the surface. It cost the whole order —
1.59 against the 2.99 the same mesh gives once the surface borrows
instead. A fallback that works is harder to spot than one that fails.

**Three test errors, each of a different kind.** The mid-side node
ordering for Hex20 was plausible and not the element library's, which
built hexes that were inside out at half their quadrature points. The
principal-stress invariants were scaled by the largest principal value
rather than the largest *magnitude*, so a wholly compressive state
divided by something near zero and reported a relative error of order one
for a correct eigenvalue. And I asserted that Mohr–Coulomb with equal
strengths is Tresca, which is only true where the state straddles zero —
with both extreme principal stresses positive the two criteria make
genuinely different statements, and that is the criterion rather than a
bug in it.

---

### J.2: how you test a picture

Not by looking at it. Every routine produces geometry, and geometry has
properties that follow from what it is supposed to be.

| what was measured | result |
|---|---|
| decomposition volume, Tet4 / Tet10 / Hex8 meshes of the unit cube | **1.000000000000003**, 0.999999999999968, 1.000000000000003 |
| boundary area and enclosed volume of each | **6.0** and **1.0** to 5e-14 |
| surface of a 4-per-side cube | 98 vertices, 192 triangles, watertight |
| normals | unit to 3.6e-08, every one pointing outwards |
| auto scale | largest movement **0.080000** of the diagonal |
| clipped at x = 0.5 | area **3.000000000000** |
| section normal to x / to (1,1,0) / to (1,1,1) | **1.0**, **1.414213562373**, **0.866025403784**, exact to 1e-12 |
| isosurface of a radius field at r = 0.3, 8/16/32 per side | area error 4.56%, 1.12%, **0.28%**, rate **2.017** |
| its enclosed volume | 0.1031, 0.1107, 0.1125 against 4/3 pi r^3 = 0.1131, rate **2.012** |
| the same sphere on Tet4 and Tet10 meshes | 2.01% and 0.44%, both watertight |

**A cut plane is an isosurface of the signed distance to the plane**, so
there is one polygon-through-a-tetrahedron routine and not two. The
second copy is where the two would have come to disagree about a
degenerate case.

**Marching tetrahedra has no ambiguous case.** Marching cubes has fifteen
cases, several admitting two topologies, and choosing between them
inconsistently in neighbouring cells leaves holes in a surface that is
supposed to be closed. A tetrahedron admits exactly two shapes and
neither has a choice in it, so watertightness is by construction. It is
still checked, because "by construction" is a claim about code that was
written by hand.

**The triangles were not consistently oriented, and only the volume said
so.** The area of the sphere was right to a fraction of a percent while
the enclosed volume read 0.044 against its true 0.113, because the
marching cases emitted whatever winding fell out of the case analysis.
Area uses a magnitude and does not care; the divergence theorem does. Two
sided lighting would have hidden it in every picture. The fix needs no
case analysis: the vector from a vertex below the level to one above it
is the direction the normal must have.

**Area and volume converge from *below*, and that is checked too.** The
triangulated surface has its vertices on the sphere and its faces inside
it, so an inscribed polyhedron cannot have more area or enclose more
volume than the sphere. A result that overshot would mean a triangle
counted twice or wound backwards, and a magnitude-only check would pass
it.

**And the self-check numbers are computed in double precision, not read
back out of the float arrays.** A renderer wants floats; an area summed
over ten thousand of them carries about seven digits, which is plenty to
draw with and not enough to compare against a closed form. The diagonal
section came out 2.5e-09 wrong for no other reason.

Two smaller ones worth keeping: the six-tetrahedron hexahedron split must
share the 0-6 diagonal in *every* element, or neighbours cut their common
face along different diagonals, the interior faces fail to cancel, and
the solid is drawn full of internal walls. And a clip that keeps every
triangle with one vertex across the plane leaves a slab of the discarded
half attached -- on a cube cut down the middle, an extra ring of area
exactly one, which looks like a rendering artefact and is arithmetic.

### J.3 and J.4: frames, probes, and the numbers leaving the program

| what was measured | result |
|---|---|
| mode animation, every frame against cos(2 pi k / N) times the shape | **1.1e-17** of a 3.25e-02 peak |
| frames half a cycle apart | exact negatives; the 24th continues into the 0th exactly |
| peak movement | **0.080000** of the diagonal, as asked |
| period | the mode's own, 1/312.10 Hz |
| history resampling, 9/17/33 samples onto 101 frames | 1.46e-02, 4.00e-03, 9.62e-04, rate **1.963** |
| a transient recorded at 21 fixed times | steps ranged **0.31 s to 476 s**, all 21 recorded |
| its first and last samples | the initial condition and the returned answer, **exactly** |
| probes of a linear field at corners and interior points | error **0.00e+00** |
| a point two units clear of the model | reported outside, distance **2.028944** |
| 41 path samples over two unequal segments | spacing even to 9.0e-17, values exact to 1.8e-15 |
| a time history at two points against a closed form | error **0.00e+00** |
| CSV round trip | every value **bit-exact** |

**A mode shape has no magnitude, so one has to be imposed.** It is an
eigenvector; `SolveModal` returns it mass-normalised, which makes the
participation factors mean something and makes its amplitude a number in
units of one over root mass, with no reading as a displacement at all.
Drawn at "true scale" it would be invisible for a heavy model and off the
screen for a light one, *with the same mode shape in both*. The amplitude
comes from the geometry; the period does not, and is the mode's own, so
two modes animated side by side move at their true relative speeds.

**The transient's steps ranged over a factor of fifteen hundred**, 0.31 s
to 476 s, which is the whole argument for recording at asked-for times.
A history taken at the integrator's own steps is unevenly spaced, changes
shape when the tolerance does, and is no way to drive a frame rate. The
field is interpolated within the bracketing step rather than snapped to
the nearer end: snapping converges at first order, and the rate test
catches it where the values would not -- an animation that holds still
and then jumps reads as a solver artefact and is not one.

**`MapReport::worst_distance` was the Newton residual, not a distance.**
The two look alike -- both come back from `InverseMap`, both are small
when all is well -- and an isoparametric map extrapolates perfectly
happily, so a point two whole units clear of the model converges to a
reference coordinate outside the element with a residual of zero. The
field read zero for exactly the case it exists to report, and had done
since I.3. It now measures from the point to where it landed after
clamping, which is the closest point of the element it took its value
from.

**Seventeen significant digits in the export, not six.** A result exported
to be plotted can spare the characters; a result exported to be
*compared* -- against another run, another code, an earlier version of
this one -- is worthless rounded, because the difference being looked for
is usually smaller than six digits keeps. And every history column is
named after a point, and a point has three commas in it: unquoted, "von
Mises at (1, 2, 3)" becomes three columns and the file still parses, into
a table with the wrong shape.

**Two test errors, both of the same kind: an index that was not the thing
it was named.** Half way through the node array of a bar meshed 20 by 1
by 1 is node 42, which is on the *end* face and held at zero, so the
"centre" history was zero throughout and every check of it passed for the
wrong reason. And the first recorded sample is not the initial condition
at the held nodes -- a prescribed temperature is imposed at the first step
rather than blended in, which is the physics -- so comparing all nodes
against the initial field tests the wrong thing. Separately, the first
version ran the transient for forty seconds: steel's diffusivity is
1.39e-05 m^2/s, so a half-metre bar has a time constant of 1830 s, the
centre was still at its initial hundred degrees at the end, and the test
measured nothing at all. Four thousand seconds takes it to 14.18 K.

### J.5: the estimator works; the mesher is what stops the loop

| what was measured | result |
|---|---|
| effectivity index, 4 / 8 / 16 per side | **0.9601 -> 0.9954 -> 0.9993** |
| per-element agreement with the true error | r = 0.9979, 0.9995, **0.9999** |
| the true energy-norm error's rate | **1.001** (a linear element gives 1) |
| relative error reported on those meshes | 34.9%, 16.0%, **7.8%** |
| refinement, targets of 90% / 2% / 0.5% | **0**, 64, 64 of 64 elements |
| every refined element against every untouched one | more error, without exception |
| the loop on a real body | 333 -> 1917 elements, 14.46% -> **12.64%** |
| strain energy over those cycles | 1.2966e+04 -> **1.3092e+04**, increasing |
| cycle 3 | **could not be meshed**: worst dihedral 1.80 degrees |

**The effectivity index is the whole test**, and it is the reason this
part has a manufactured solution in it. An error estimator is a claim
about a quantity nobody can measure; the only way to check it is on a
problem where somebody can. An estimator that is merely small when the
error is small is worth nothing — so is a constant — and what matters is
that the ratio approaches *one*, which decides whether the percentage it
reports can be believed as a percentage or only as a ranking. 0.9993 on
the finest mesh says it can. The per-element correlation of 0.9999
matters separately: a global index near one with no per-element agreement
would refine the wrong elements while reporting the right total.

**The strain energy increasing is the check that a refinement sequence is
one.** A displacement formulation is too stiff, always, so every
refinement releases a little more energy and the sequence must rise. A
load that changed with the mesh, or a support that resolved to a
different face between cycles, breaks it immediately — which is exactly
the failure a loop that re-meshes from CAD each cycle is exposed to and a
loop that subdivides is not.

**And this is where Part J stops being able to prove its own case.** On
the third cycle the graded sizing field the refinement asks for produces
tetrahedra with a worst dihedral angle of 1.8 degrees, and the mesher
declines them. The loop stops, reports why, and returns the last good
cycle — which is the right behaviour and is not a demonstration. Against
a uniform mesh of comparable size the adaptive one came out *behind*,
12.66% at 2048 elements against 10.29% at 2307, and both reasons are
worth separating: a plain block under uniform pressure has no stress
concentration to find, so the best mesh genuinely is the uniform one and
an adaptive loop that disagreed would be wrong; and the loop did not get
to spend its budget in any case. Showing the payoff needs a geometry with
a concentration *and* a mesher that grades cleanly, and the second is
G.4's gap rather than this one's. Recorded rather than arranged around.

Smaller things the writing turned up. The error integrand is a difference
of two stress fields of *different* polynomial order, so it is not what
the element's own quadrature rule was chosen to integrate exactly, and
under-integrating it flatters precisely the elements where the two fields
disagree most; it is integrated two degrees higher. The compliance matrix
is obtained by inverting the constitutive one rather than written out,
because the closed form for an isotropic material is only that, and a
study's materials may be orthotropic or temperature dependent. And the
refinements are rebuilt from the original options each cycle rather than
accumulated: they are absolute sizes at absolute places, so a stale
coarser request sitting under a newer finer one is harmless — a sizing
field takes the smaller — but the list would grow without bound and a
cycle's mesh would stop being a function of that cycle's estimate.

## Part K — API, agent surface and docs

- [x] **K.1 Lua bindings.** `mep.part_*` and `mep.fem_*`, generated in
  `src/lua_env.cpp` from the one method table rather than written out.
  Failure is nil plus a message, this file's convention for an operation
  that can legitimately fail on its input.
- [x] **K.2 Agent RPC.** `part.*` and `fem.*` in `src/agent_rpc.cpp`,
  matched against the table.
  - [ ] **The namespace is `part.*`, not `cad.*` as this plan said.**
    `cad.*` was already taken by Part F.5's in-pane CAD buffer, with
    fifteen methods and three direct collisions. See below.
- [x] **K.3 MCP tools.** `mep_part_*` and `mep_fem_*`, generated in
  `src/mcp_bridge.cpp` from the same table, schemas and all. The table
  lives in its own translation unit (`src/cad_fem_methods.cpp`) with no
  kernel dependency, so `mep-mcp` stays what it was meant to be -- a
  plain socket client -- rather than linking a B-rep kernel to declare a
  tool.
- [x] **K.4 Documentation.** `MEP_AGENT_API.md`'s "CAD and finite
  elements" section and `help/cad-fem.org`'s reference are **generated**
  from the table by `just cad-fem-docs`, and `mep-cad-fem-api-test` fails
  if the checked-in files are not what the generator would write. Plus
  `docs/cad-fem.org` (architecture, written by hand) and a
  `.claude/skills/mep-cad-fem` skill.
- [x] **K.5 CLI.** `mep --cad-fem <script.json> [out.json]`, its synonym
  `mep --fem-solve`, and `mep --cad-export <in> <out>`. A script is a
  list of the same calls with the same parameters; naming one with `"as"`
  lets later calls write `$name.field`, so no handle is ever written
  down.
  - [ ] No `.mepcad` save or load through this surface, and it is not
    just a missing binding. `.mepcad` stores a *feature tree* -- that is
    the whole point of the format, since a file of faces cannot have its
    extrude distance changed -- and this session holds an evaluated
    `cad::Model` built by primitives and booleans, with no history to
    write. Giving it one means recording the calls as features, which is
    a design change rather than a binding.

- [x] **K.6 What the examples needed.** `examples/nafems` drives this
  surface and only this surface, and building it added five methods that
  were missing rather than wrong: `part.edges` and `part.edge_at` (a
  simple support is a *line*, and expressed as a face it becomes a
  built-in end); `fem.nodes` (a mode identified by what it is needs the
  shape and the coordinates in the same indexing); and `fem.movie` and
  `fem.image`. `fem.mesh` gained `method="sweep"`, which put Part G.6's
  hexahedral sweeper on the surface for the first time, and
  `part.import` learned to read a `.mepcad` feature tree, which is what
  lets one file open in the CAD pane and be analysed headlessly.
  `mep --run-lua` runs such a script with no display. See
  `plans/NAFEMS_PLAN.md`, "The examples, and what they found", for the
  five defects they turned up.

All of it over one shared session in `src/cad_fem_api.{h,cpp}`, covered
by `mep-cad-fem-api-test` (645 checks) and `mep-cad-fem-api-live-test`,
which drives a real running mep over the socket, through Lua and through
the CLI.

### Part J.3's other half, and Part K's sixth surface

`fem_animate.h` produced the frames of an animation and deliberately
stopped there, on the grounds that the camera, the keyframe track and the
video encoder already existed for the 3D modeller and a second copy of
any of them would be a second copy to keep working. That was right about
the encoder and wrong about the renderer, and `src/fem_movie.{h,cpp}` is
the difference: a software rasteriser of about two hundred lines --
z-buffer, perspective-correct interpolation, two-sided Gouraud shading,
a five-by-seven typeface for the colour bar -- feeding the same
`jpeg::Encode` and `mov::WriteMovFile` the modeller's export uses. The
file that comes out opens in mep's own video pane.

**Not through `Model3DRenderAnimationToVideoFile`**, for two reasons and
the second settles it. Its animation is a track of *transforms*, and a
mode shape is not a transform of anything -- every node moves
differently, which is what makes it a mode shape rather than a rigid
motion. And it renders through OpenGL from a live editor buffer, while a
benchmark that produces its own film has to run in a test and on a
machine with no display. The rasteriser has no dependencies, renders the
same picture on every machine, and can therefore be *asserted about*:
`mep-fem-movie-test` checks that a cube seen face on is square and
centred, that a solid four units behind another contributes exactly zero
pixels, that the colour bar's ends are the colour map's ends, and that a
frame read back out of the container decodes to within about one level of
the pixels that went in.

The one non-obvious thing in it is what is held *fixed* across a film.
The camera is fitted once, from the undeformed shape, and the
displacement scale is settled once over every frame. Fitting either per
frame produces an animation in which nothing appears to move -- the thing
that moved and the frame around it moved together -- and it is the single
easiest way to get a mode animation subtly and unaccountably wrong. When
the camera orbits, the fit is the worst case over the whole circle, so a
long part does not breathe as it turns edge on. There is a test for each.

`fem.movie` and `fem.image` are on the method table like everything else,
so they are Lua, RPC, MCP and documentation at once.

- [ ] Nothing here is wired into an editor *pane*. Part J.2's renderer
  returns arrays and no pane assembles them, so a result can be computed,
  probed and exported from the editor but not looked at in it.
- [ ] Sketches, extrudes and the feature tree are not on this surface;
  they are on Part F.5's in-pane `cad.*` one. The two do not share a
  document.

### Part K: one table, four surfaces, and a namespace collision

| what was measured | result |
|---|---|
| methods declared | **29**, every one implemented and reachable |
| methods reachable over the agent socket | **29 of 29** |
| methods bound in Lua | **29 of 29** |
| MCP tools generated | **29**, with schemas, from the same table |
| methods refusing a call with no arguments | **27 of 27** that have required ones |
| a block's volume, area, centroid | exact to 1e-12 |
| its inertia against `m(b^2+c^2)/12` | **1.2e-15** relative, off-diagonal 1.7e-18 |
| a 10 mm hole, tessellated | **6.2e-06** relative, and high rather than merely near |
| STEP written and read back | **24.000000000** against the 24.0 that went in |
| the headless CLI against the socket | **bit-identical** displacement |

**The rule was one implementation and three adapters**, and it earned
itself immediately: the Lua bindings, the RPC dispatch, the MCP schemas
and the reference documentation are each a loop over one table. The test
asserts the properties that make that worth having -- every declared
method is implemented, every one is routed, every one is bound, and the
checked-in documentation is what the generator would write. Four
hand-maintained lists would have been four chances to drift, and the
fourth is always the documentation.

**The table is in its own translation unit with no kernel dependency**,
which is not tidiness. `mep-mcp` is deliberately a plain external client
of the agent socket -- it links none of mep_core -- and it generates its
tool declarations from this table. Had the table lived beside the session
that implements it, declaring thirty tools would have meant linking the
entire B-rep kernel and finite-element stack into a process whose whole
job is forwarding JSON down a socket.

**The namespace in this plan was wrong, and the routing that implemented
it was worse.** Part K said `cad.*`; `cad.*` had been taken since F.5 by
the in-pane CAD buffer, with fifteen methods of which `cad.new`,
`cad.info` and `cad.export` are direct name collisions. My first dispatch
claimed the whole `cad.` prefix and so silently shadowed all fifteen --
three ambiguously and twelve into unreachability. The routing now matches
against the table rather than a prefix, and the kernel session answers to
`part.*`, because the headless document it operates on and the CAD buffer
the editor opens are genuinely different things and calling both of them
`cad` would have left the surface ambiguous even once the routing was
exact. `mep-cad-live-test` and `mep-cad-sketch-live-test` both pass
again; neither would have, and neither is run by `just test`.

**`cad::ComputeMassProperties` integrates a surface's whole parameter
domain**, which is the trap its signature cannot express and which the
STEP round trip found. A `MassFace` is a surface and a sense; it carries
no trimming, so there is no way for it to know which part of a surface a
face uses. For a body the kernel just built that is right, because
`MakeBox` hands each face a plane whose domain is the face -- so every
test of it passes. For the same box written to STEP and read back, the
planes come back untrimmed with domains of -1e5 to 1e5 and a 24 m^3 solid
measured **1.2e11**. The failure is silent and depends on where the body
came from rather than on what it is. `part.mass` now integrates the
tessellated boundary, which honours trimming by construction, and
`cad_mass.h` says all this where the next caller will read it.

**Every solve reports its own estimated error**, which is the one design
decision here I would defend hardest. `fem.solve` runs J.5's estimator
and returns `relative_error` whether or not anyone asked. A stress number
handed to something that will act on it, with no indication of how
converged it is, is the most dangerous output this whole system produces,
and it costs one pass over the elements to say.

Smaller ones. The chord tolerance and the mesh size default to a fraction
of the body's own diagonal, because an absolute default is right for
exactly one size of model and never the one in front of you -- an
absolute 1e-2 gave a 10 mm hole a handful of facets and a volume 0.035%
high. Handles share one counter across documents, studies, meshes and
results, so a mesh handle passed where a study is wanted fails instead of
quietly finding whatever had that number. Closing a document closes
everything built on it. And the headless CLI is a script of the existing
calls rather than a declarative study format, because a study format
would have been a *fifth* description of the same operations and the
fifth is the one that would be missing whatever was added last.

---

## The sweep: fixing what the parts left behind

Five fixes, and the useful part is what each of them was *not*.

| what changed | effect |
|---|---|
| boundary nodes slide along their own CAD face or edge during repair | 1.5 elements across a thin slab: **4.28 degrees and refused -> 13.20 and accepted** |
| smart-Laplacian guard on every smoothing move | 4 and 6 elements across: 13.6 -> 15.7, 13.5 -> 15.0 |
| repair repeated while it still helps | graded meshes get a second and third round |
| coincident surface nodes welded | the L-bracket's 4.2e-08 edge is gone; its baseline mesh 8.91 -> 11.31 degrees |
| an exact persistent-name match wins outright | **an L-bracket can carry a study at all** |
| the refusal names the stage actually at fault | `min_surface_angle` is reported and quoted |

**The mesher is not scale dependent, and I had said it was — twice, in
this plan and in a note to myself.** Measured directly: the same box at
scales from 100 down to 0.001, with the target size held proportional,
gives worst dihedral angles of 15.8, 15.9, 15.8, 20.3, 16.1, 15.5. It is
scale invariant to within the noise of the algorithm. The failure I had
attributed to scale was a *ratio*: a fractional number of elements across
a thin dimension. 1.0 and 2.0 elements across a slab are fine; 1.5 is
not. A wrong diagnosis in a plan is worse than no diagnosis, because it
sends the next reader to the sizing field, which was never the problem.

**Freezing the boundary is what made thin parts unfixable.** The
smoothing pass skipped every boundary node, on the sound-looking grounds
that a boundary node moved is a boundary changed. A slab two elements
thick has no interior nodes at all, so the pass did nothing, and a sliver
whose four nodes are all on the boundary has no interior face to flip
across either. The boundary need not be frozen, only *respected*: Part
G.2 already recorded which face, edge or vertex each node came from, and
the exact surface is still there to re-evaluate, so a node can slide
along its own geometry and land on the true surface rather than on the
chord it was sitting on. The same argument as G.5's mid-side projection,
and the same payoff for having kept the B-rep.

**Then it had to be made a repair rather than an improvement, and a test
failure is what said so.** Letting every boundary node slide gives better
quality numbers across the board and broke the cylinder under external
pressure: that model is held by symmetry, its restraint selects the nodes
with `x == 0`, and a node on the curved face slid tangentially along the
cylinder — staying perfectly on the geometry and leaving the symmetry
plane. The solve came back as a singular matrix. **Boundary conditions
are routinely attached by position, and the mesher cannot know which
positions a caller cares about**, so a node may now move only when the
element it belongs to would otherwise be refused outright. Every mesh
that was already acceptable is untouched.

**A perfect name match was being refused.** `ResolveFace` requires the
best candidate to beat the runner-up by 15% of its score, on the stated
grounds that asking costs a click and guessing wrong costs a silently
incorrect part. That is right when choosing between approximations and
wrong when one candidate matched *exactly*: an L-bracket's held face
scored 1.000000 against 0.873604, a gap of 12.6%, so a support attached
to that face and resolved against **the very model it was captured from**
came back ambiguous. Simple boxes never showed it because their
runners-up score far lower. An exact match now wins outright; two
candidates both scoring one stay ambiguous, which is the case the caution
was actually written for.

**And the remaining limit is the surface mesher, not the volume one.**
With a 3x local size reduction — what an adaptive cycle asks for — the
surface mesher emits collinear nodes and a zero-area triangle, and the
volume mesher then reported a *quality* failure. It cannot repair a
degenerate input and should not be blamed for failing to, so the refusal
now measures the worst boundary-triangle angle and says outright when the
surface is no better than the volume. That cost most of an afternoon to
find, largely because the message pointed at the wrong file.

So the adaptive loop still stops early on an L-bracket, and the reason is
now precisely located rather than guessed at: Ruppert refinement with
proper encroachment handling, recorded as absent since G.2. That is a
feature, and the honest thing is to leave it named rather than to start
it here.

**Two stale claims removed from this plan**, both of which had been
untrue for some time. `fem_solve.cpp` no longer carries its own Hex8; it
calls the element library. And `fem::Model`, `fem_solve.cpp` and the
`mep-fem` process are not "the path the editor uses" — nothing in the
editor, in Lua or in the justfile spawns `mep-fem` at all, and
`main.cpp` never mentions `fem::Model`. Part K's session superseded them.

---

## Verification strategy

This repo's established pattern is to verify an in-house engine against
the real tool on real inputs — formatters against `clang-format` and the
language's own lexer, LSPs against the language's own parser, the browser
engine against Chromium, the maths renderer against MathJax, the PDF
renderer against `pdftoppm`. The same applies here, and the references
are unusually good:

| Layer | Oracle |
|---|---|
| NURBS algorithms | Naive direct implementations; finite-difference derivatives; geometric invariance under knot/degree operations |
| Booleans | Volume conservation via A.6; OpenCASCADE on identical randomised inputs |
| STEP | Round-trip through FreeCAD; mass properties and topology counts preserved |
| Meshing | Gmsh/netgen quality distributions; Jacobian positivity; boundary closure |
| Element formulation | Patch tests (every element, mandatory) |
| Linear static | NAFEMS LE series; manufactured solutions with measured convergence order; CalculiX cross-check |
| Modal | NAFEMS FV series; Sturm sequence count |
| Nonlinear/contact | NAFEMS contact benchmarks; analytic Hertzian contact; energy balance |

Headless test targets follow the existing convention (`EXCLUDE_FROM_ALL`,
registered against the `tests` target, run by `just test`) — note
`CHECK()`, never `assert()`, since release builds define `NDEBUG`.

---

## Risks, honestly stated

1. **Surface–surface intersection (C.3) is the project.** Every B-rep
   kernel's robustness reputation is decided here. Mitigation: the
   analytic table in C.2 keeps the general path off the common path; the
   validity checker in B.3 catches bad output at the source; C.5's
   fuzzing runs continuously rather than once.
2. **Fillets (E.3) fail on real models.** Every commercial kernel's
   fillets fail on real models too. The mitigation is explicit
   preconditions and honest failure reporting, not silent approximation.
   Borne out: the first attempt at E.3 was accepted only because a single
   box size happened to work, and what shipped instead states its
   preconditions up front and refuses by name. Each refusal names the one
   thing that was wrong -- and telling "this radius will not fit" apart
   from "this edge will not work" turned out to need arithmetic of its
   own, because the obvious call for it silently clamps.
3. **Persistent naming (B.4) is a research-grade problem.** No system
   solves it completely. Design for graceful degradation — report an
   ambiguous reference to the user rather than rebuilding something wrong.
4. **Scope.** Part A through Part C is a kernel; Part G through Part I is
   a solver suite. Either alone is a large project. The 0.6 vertical slice
   exists specifically so that there is a working, demonstrable pipeline
   long before either is finished, and so that Parts G–H can proceed on
   imported STEP geometry while Parts D–E are still being built.
5. **Precision discipline.** The kernel is double throughout and converts
   to float only at B.5. A float leaking upward will manifest as an
   intermittent boolean failure weeks later.
