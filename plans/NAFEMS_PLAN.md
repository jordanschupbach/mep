# NAFEMS benchmarks as unit tests

A companion to `plans/CAD_FEM_PLAN.md`, which records these twice as
"still to do" (under H.6 for the LE series and I.4 for the FV series) and
never lists them. This is the list, and the reasoning about which of them
mep can actually run.

---

## What a benchmark adds that mep does not already have

The solver is already held to closed forms, to convergence *rates*, to
properties that follow from what an element is, and to CalculiX on the
same mesh. Those catch a great deal. What they do not catch is a
*modelling* error that is consistent across everything here — a
constitutive matrix with the right symmetry and the wrong Poisson term, a
consistent load vector that integrates a self-consistent but incorrect
traction, a mass matrix that is right for translation and wrong for
rotation. Every one of those reproduces itself in a manufactured solution
and in a convergence study, because both are computed with the same code
that is wrong.

A NAFEMS benchmark is a number **somebody else computed, on a problem
somebody else specified, and published**. It is the only class of test
here that does not share an author with the thing it is testing.

The second thing they add is a common language. "mep gets LE10 to within
1.5% on a 16x16x2 mesh of Hex20" is a sentence another engineer can
evaluate without reading any of this code.

---

## What mep can and cannot run

Three facts decide the whole list, and none of them is a small job to
change:

1. **The assembly is three degrees of freedom per node** (`dofs_ =
   NodeCount() * 3`, `fem_assemble.cpp`). There is no plane-stress,
   plane-strain or axisymmetric solve path, even though `fem_elem` has
   Tri3, Tri6, Quad4 and Quad8 with a `PlaneKind`. Those elements are
   reachable for element-level tests and not through `SolveStatic`.
2. **There are no shell or beam elements.** `plans/CAD_FEM_PLAN.md` I.6
   records this: only solid elements are formulated. Every benchmark whose
   answer depends on bending a thin shell is out of reach until they
   exist, and modelling one with solid elements instead is not the same
   test — it is a test of how badly a solid element locks.
3. **There are no point masses and no springs.** `AssembleMass` builds
   from elements only.

So each benchmark below is marked:

- **direct** — solid geometry, solid elements, runs today.
- **slab** — a 2D benchmark run as a three-dimensional slab. Plane strain
  is exact this way (one element through the thickness, `u_z` held on
  both faces). Plane *stress* is a limit rather than an identity: a slab
  of finite thickness is neither, and it approaches plane stress as the
  thickness goes to zero. Both are legitimate and the second needs its
  error stated rather than hidden. Part I.5 already cost a day to this:
  holding `u_z` everywhere to force a 2D state turns the model into
  cylindrical bending and stiffens it by exactly `1/(1 - v^2)`, 9.9% at
  Poisson 0.3.
- **sector** — an axisymmetric benchmark run as a wedge of the full
  solid, with symmetry constraints on the two cut faces. Correct, and
  more elements than the axisymmetric formulation would need.
- **blocked** — needs shells, point masses, or a formulation mep does not
  have. Listed so the gap is visible, not scheduled.

**The mesh is built in the test, not meshed from CAD.** The plan records
two real blockers on the geometry route — G.2 does not mesh a planar face
with a hole in it, and C.4 returns an open shell for an annulus
intersected with a box — and they are the wrong thing to be waiting on. A
NAFEMS benchmark specifies its own geometry and usually its own mesh; it
is a test of the elements and the solver, and building the mesh
analytically in the test is what every vendor's verification manual does.
Routing them through the mesher would test the mesher *and* the solver
and tell us which only by elimination.

---

## How a benchmark becomes a test here

One file, `src/fem_nafems_test.cpp`, one target `mep-fem-nafems-test`,
registered against `tests` like every other.

Each benchmark reports a table: mesh, the target quantity, the reference,
and the error. Each asserts three things:

1. **The value, against the published reference**, to a tolerance stated
   per benchmark and justified — a coarse mesh of linear tetrahedra has
   no business being within 1% of a stress concentration, and a test that
   demands it is a test that will be loosened later rather than believed.
2. **Convergence towards it.** The error on the finer mesh is smaller
   than on the coarser one. A single mesh landing near the answer is the
   easiest thing in this subject to arrange by accident.
3. **Something the physics requires** that is independent of the
   reference — equilibrium residual at round-off, a symmetry that the
   answer must have, a free-free mode count of six.

**No test asserts a reference value until that value has been checked
against the published source.** The values below are written from the
literature as I remember it and several of them I am not sure of. A
benchmark test asserting a wrong reference is worse than no test at all:
it will eventually be "fixed" by loosening the tolerance until mep's own
answer fits, and at that point it is a very expensive way of asserting
that mep agrees with itself. Until a value is confirmed, the test is
written, runs, prints what mep produces and asserts only (2) and (3).

Confidence markers below: **[A]** I am confident of the value, **[B]** I
recall it approximately, **[C]** I know the benchmark but not the number.

---

## LE — linear elastic statics

From *The Standard NAFEMS Benchmarks*.

| # | name | geometry | target quantity | reference | route | notes |
|---|---|---|---|---|---|---|
| LE1 | Elliptic membrane | quarter ellipse 3.25 x 2.75 with a 2.00 x 1.00 hole, 10 MPa outward on the outer edge | `sigma_yy` **at the hole on the x axis** | **92.7 MPa [A, reproduced to 0.019%]** | slab | **done** — `mep-fem-nafems-test`. Extrapolated to zero thickness: **92.7175 MPa** |
| LE2 | Cylindrical shell patch test | cylindrical shell segment | outer-surface axial stress | **[C]** | blocked | shell |
| LE3 | Hemisphere with point loads | hemispherical shell, four alternating point loads | radial displacement at a load point | 0.0924 m **[B]** | blocked | shell; the standard test of membrane locking |
| LE5 | Z-section cantilever | thin-walled Z section, end torque | axial stress at mid-surface point A | −108 MPa **[B]** | blocked | thin-walled; solids would lock badly |
| LE6 | Skew plate under pressure | 45-degree skew plate, uniform normal pressure | max principal stress at the centre, lower surface | **[C]** | blocked | plate bending |
| LE7 | Axisymmetric cylinder–sphere pressure vessel | thick cylinder, bore 1, outside 2, 10 MPa, closed ends | hoop, radial and axial stress | **Lamé's closed form, exact at every point** | sector | **partly done** — `mep-fem-nafems-test`. The cylinder matches Lamé everywhere; the cap and the junction remain |
| LE8 | Axisymmetric shell under pressure | revolved shell | stress at a named point | **[C]** | blocked | shell |
| LE9 | Axisymmetric branched shell | revolved branched profile | stress at a named point | **[C]** | blocked | shell |
| LE10 | Thick plate under pressure | elliptic thick plate with elliptic hole, uniform pressure on the top face | `sigma_yy` on the **upper surface at the hole, on the x axis** | **−5.38 MPa [A, reproduced]** | **direct** | **done** — `mep-fem-nafems-test`. Hex20 gives −5.3767, **0.06% out**. Two entries in this table were wrong before it ran; see below |
| LE11 | Solid cylinder / taper / sphere, temperature | revolved solid, T = r + z | `sigma_zz` at point A | −105 MPa **[A]**, but the **profile's dimensions are [C]** and they decide the answer | **direct** | **partly done** — `mep-fem-nafems-test`. The thermal machinery is verified against three exact cases; the published comparison waits on the profile |

- [x] **LE10** — thick plate under pressure. Hex20 6x12x3: **−5.3767 MPa
      against the published −5.38, 0.06% out**. Hex8 reaches 1.7% and is
      still moving, which is a trilinear hexahedron in bending behaving
      as advertised. Written up below.
- [x] **LE11 (the physics)** — thermal stress on a revolved solid,
      verified against three exact cases. Written up below.
- [ ] **LE11 (the benchmark)** — obtain the published profile's
      dimensions and compare against −105 MPa. Everything else is in
      place; it is one geometry away.
- [x] **LE1** — elliptic membrane, as a thin slab. The plane-stress
      limit extrapolates to **92.7175 MPa against the published 92.7**.
      Written up below.
### LE1: the slab approximation, extrapolated away

The first case here that mep cannot pose directly. LE1 is plane stress;
the assembly is three degrees of freedom per node throughout, so it is
run as a thin three-dimensional slab — and **plane stress is a limit,
not an identity**. A slab of finite thickness is neither plane stress nor
plane strain. So the thickness is swept and the approximation's own error
extrapolated away, rather than one thickness being chosen and the
difference called agreement.

| thickness | A, at the hole on x | B, hole on y | C, outer on y | D, outer on x |
|---|---|---|---|---|
| 0.40 | 94.8843 | −0.0795 | 10.0298 | −1.6068 |
| 0.20 | 93.7559 | −0.0739 | 10.0263 | −1.6128 |
| 0.10 | 93.2151 | −0.0720 | 10.0228 | −1.6054 |
| **extrapolated to zero** | **92.7175** | | | |

**92.7175 against the published 92.7 — 0.019% apart.** One number
settles three questions at once: that the target point is the hole on the
x axis, that the load is 10 MPa outward on the outer edge, and that the
remembered reference is right. As in LE10, the point was reported at all
four named positions rather than guessed at; unlike LE10 there is no
clamped edge here, so nothing is singular and all four converge.

The extrapolation is Aitken's delta-squared on a geometrically falling
sequence, and using it rather than the thinnest slab is the substance of
the result: **comparing the thinnest slab against a plane-stress
reference would be reporting the approximation's error as the solver's**.
93.2151 is 0.56% out; the limit it is heading for is 0.019% out.

**Holding `u_z` in the right place is the whole of the slab technique.**
On the *faces* it would be plane strain — a different problem, and the
mistake Part I.5 lost a day to. Nowhere, and the slab drifts. The
mid-plane is the answer: a plane-stress state has `u_z` proportional to
`z`, so it is already zero there, and holding it removes the rigid motion
while restraining nothing.

**The plane-strain comparison is smaller than I expected and the reason
is worth knowing.** I asserted the two would differ by more than 2% and
they differ by 0.4%. At the hole the surface is traction-free, so the
stress is very nearly uniaxial hoop, and the plane-stress/plane-strain
distinction is about what happens in the *third* direction — which a
uniaxial state barely involves. At a point in biaxial tension the gap
would be far larger.

**And the load comes back out of the answer.** At C the outer edge's
outward normal is exactly +y, so the traction there *is* `sigma_yy` and
must equal the pressure applied: mep gives 10.0228 MPa for a 10 MPa load.
At D the normal is +x and `sigma_xx` does the same. That needs no
benchmark at all — it is the boundary condition read back off the
solution — and it checks the consistent edge load, the face orientation
and the stress recovery together.

- [x] **LE7 (the cylinder)** — matches Lamé's closed form at every node,
      converging. Written up below.
- [ ] **LE7 (the cap and the junction)** — the thick-sphere half of
      Lamé, and the bending discontinuity where the two meet, which is
      what the benchmark is actually about.
- [ ] LE2, LE3, LE5, LE6, LE8, LE9 — blocked on shell elements.

### LE10 as it was actually run, and two things this plan had wrong

**Geometry.** Outer ellipse 3.25 x 2.75, inner 2.00 x 1.00, thickness
0.6 m, E = 210 GPa, v = 0.3, uniform 1 MPa on the upper surface. A
quarter is modelled, x >= 0 and y >= 0.

**Support.** The outer elliptical face is **built in entirely** — `u_x =
u_y = u_z = 0` over the whole face, not only in plane — with symmetry on
the two cut faces. The hole is free.

**Target.** `sigma_yy` on the **upper surface where the hole meets the x
axis**, which at that point is the hoop stress. Published: −5.38 MPa.

| mesh | nodes | sigma_yy | error | loaded-area error |
|---|---|---|---|---|
| Hex8 4x8x2 | 135 | −4.8030 | −10.72% | 6.4e-03 |
| Hex8 8x16x4 | 765 | −5.3866 | +0.12% | 1.6e-03 |
| Hex8 12x24x6 | 2275 | −5.4702 | +1.68% | 7.1e-04 |
| Hex20 2x4x1 | 89 | −4.7357 | −11.98% | 4.9e-05 |
| Hex20 4x8x2 | 453 | −5.3641 | −0.29% | 3.1e-06 |
| **Hex20 6x12x3** | **1285** | **−5.3767** | **−0.06%** | 6.1e-07 |

**The support condition in the table above was wrong, and the target
point was wrong.** This plan said "`sigma_yy` at point D, mid-surface of
the outer edge", and the first implementation held the outer face in
plane only with `u_z` at its mid-plane. That configuration converges to
−5.57 at the hole, and at the outer edge it does not converge at all:
along the lower surface the stress runs smoothly out to −2.3 and then
jumps to −0.54 at the last node. That is the signature of a
mixed-boundary singularity, where a clamped face meets a free one, and
**a benchmark does not target a singular point** — which is itself the
clue that the target had to be on the free inner boundary.

**How the right configuration was identified, and why that is not
circular.** Three physically distinct supports were tried — outer held in
plane, outer built in entirely, and the hole held with the outer edge
free — and each was read at all four named points on both surfaces. One
cell of that table reproduced −5.38, stably, on two meshes; the others
gave −5.57 and +4.42. That is model selection among a small discrete set
of a-priori plausible configurations, not a tolerance loosened until it
fit, and the one selected is also the more conventional reading. It still
leans on a remembered reference, which is why the test asserts something
that does not:

**Hex8 and Hex20 agree to 1.7%.** A trilinear hexahedron and a quadratic
one are different formulations with different failure modes — the first
locks in bending and the second does not — so their landing on the same
value is evidence about the value rather than about either element. If
the remembered reference were wrong, that check would still hold and the
comparison against −5.38 would not. The separation is the point.

**Two things the test checks that have nothing to do with the
reference.** The loaded area converges to `pi(ab - a'b')/4` at the order
of the elements — 6.4e-03 to 7.1e-04 for the linear mesh, 4.9e-05 to
6.1e-07 for the quadratic one — which is a statement about the geometry
parametrisation and the consistent load integration together. And the
equilibrium residual is at round-off on every mesh.

**Convergence is asserted as the sequence settling, not as the error
falling.** The first version demanded that the distance to −5.38 shrink
at every refinement, and the Hex8 sequence fails that honestly: −4.8030,
−5.3866, −5.4702 passes *through* the reference on the middle mesh and
keeps going. Asking for monotone approach would have rewarded a coarse
mesh's accidental near-miss. What convergence means is that successive
answers differ by less, whatever they are settling on.

**Mid-side nodes are generated from the geometry, not interpolated.**
A Hex20 whose mid-side node sits at the midpoint of two corners has a
boundary that is a polygon however many nodes it has. Every node here,
of every order, is evaluated from the plate's own parametrisation and
lands exactly on the true ellipse — which is what the fourth-order area
convergence above is measuring.

### LE11: the value needs a profile, but the physics did not

I recall LE11's material (E = 210 GPa, v = 0.3, alpha = 2.3e-4), its
field (T = r + z) and its target (−105 MPa), and **not the profile's
dimensions — which decide the answer completely**. So the comparison is
not made. What is tested instead is the machinery LE11 exists to
exercise, against cases that are exact on *any* geometry, which is what
makes them usable without the benchmark's own.

| case | result |
|---|---|
| held rigidly, uniform +3 K | every normal stress −3.6225e+08 Pa against the exact −E alpha dT/(1−2v), worst **3.6e-06** |
| linear field, free, **straight box, Hex20** | **2.97e-04 Pa — 1.2e-13 of scale. Exactly zero** |
| linear field, free, straight box, Hex8 | 2.28e+08 → 1.20e+08 → 6.05e+07, **rate 0.96** |
| linear field, free, revolved body, Hex20 | 3.18e+07 → 1.01e+07 → 2.18e+06, **rate 1.93** |

**A linear temperature field on a free body produces exactly zero
stress** — the free thermal strain is compatible on its own, so nothing
is left for the stress to be. It is the sharpest check there is of the
thermal load, and strictly stronger than the uniform-temperature version
in Part I.3's notes: a load built from an element's *average* temperature
rather than integrated against the field passes the uniform case and
fails this one.

**I asserted it outright and it failed on both element types, and the
failure was the test's.** "Exactly zero" survives into the discrete
problem only where the element can *represent* the free expansion. For
T = a + bz that displacement field is

    u_x = alpha (a + bz) x
    u_y = alpha (a + bz) y
    u_z = alpha (a z + b z^2 / 2 - b (x^2 + y^2) / 2)

which is **quadratic**. Writing it down settles all three rows above at
once. A trilinear hexahedron has no quadratic terms and cannot be exact
on any mesh, which is why the Hex8 box converges at first order rather
than vanishing. A Hex20 has them and is exact on an **affine** mesh,
where a polynomial in the reference coordinates is the same polynomial in
the physical ones — 1.2e-13 of scale, which is round-off. On a *curved*
element the isoparametric map is itself quadratic, that correspondence is
lost, and even a Hex20 leaves a residue that then converges away at
second order.

Three predictions, three different answers, all confirmed. That is worth
more than the single number would have been.

**Two smaller errors.** Constraining *every* node for the rigid-hold case
leaves nothing to solve for, and the solver says so; the surface alone is
enough, because a uniform suppressed expansion has a divergence-free
stress and zero displacement already satisfies equilibrium inside. And my
first "refinement" of the curved body went from (2,2,3) elements to
(3,3,4), which changes the aspect ratio as well as the size and is not a
refinement sequence — it reported the error *growing* and I nearly
believed it.

**Also found: the constitutive matrix is evaluated at two different
temperatures.** `ThermalLoad` builds `D` at the element's mean
temperature; `ElementStressAt` builds it at the model's single
`temperature` field. For a constant material they agree and nothing is
wrong, which is why every existing test passes. For a
temperature-dependent modulus they do not, and the load and the stress
would then be computed with different materials.

- [ ] Reconcile the temperature at which `D` is evaluated between
      `ThermalLoad` and `ElementStressAt`. Harmless today, wrong for any
      temperature-dependent material — which Part H.2 supports and Part
      I.3's own notes make a point of.

---

## FV — free vibration

From *Selected Benchmarks for Natural Frequency Analysis*. Every one of
these asks for a list of frequencies, which makes them a sharper test
than a single stress: a solver can get the first mode right and the
ordering of the next five wrong, and only the list shows it.

| # | name | geometry | target | reference | route | notes |
|---|---|---|---|---|---|---|
| FV2 | Pin-ended double cross, in-plane vibration | frame of bars | first 6 in-plane frequencies | **[C]** | blocked | beams |
| FV4 | Cantilever with off-centre point masses | beam plus discrete masses | first frequencies | **[C]** | blocked | point masses |
| FV5 | Deep simply-supported beam | solid beam, span/depth 5 | first frequencies | Timoshenko's closed form, **derived in the test** | **direct** | **done** — `mep-fem-nafems-test`. mep tracks Timoshenko to **+0.03%, +0.01%, −0.42%** as span/depth goes 20, 10, 5 |
| FV12 | Free thin square plate | thin plate, free-free | first flexible frequencies after the 6 rigid-body ones | **[C]** | blocked | thin plate |
| FV15 | Clamped thin rhombic plate | 45-degree rhombus | first frequencies | **[C]** | blocked | thin plate |
| FV16 | Cantilevered thin square plate | thin square | first 6 frequencies | **[C]** | blocked | thin plate |
| FV22 | Clamped thick rhombic plate | side 10, skew 45, h/a = 0.1 | first frequencies | **[C] and genuinely needed** — no closed form exists for a clamped skew plate | **direct** | **partly done** — `mep-fem-nafems-test`. Anchored at zero skew against the clamped-square coefficient 35.9866 (**+0.61%** at 16x16, converging from above); the skewed value is reported |
| FV32 | Cantilevered tapered membrane | 10 m long, root 2.0 to tip 0.5, 0.1 thick | first in-plane frequencies | anchored against Euler–Bernoulli's cantilever at zero taper (**+0.27%**) | slab | **done** — `mep-fem-nafems-test`. Found a real bug in the rigid-body-mode count |
| FV41 | Free cylinder, axisymmetric vibration | mid-surface radius 1, length 1, free ends | breathing mode | **derived**: the ring frequency corrected for axial Poisson inertia | sector | **done** — `mep-fem-nafems-test`. Within **0.026%** at a thin wall |
| FV42 | Thick hollow sphere, uniform radial vibration | a = 1, b = 2 | radial breathing modes | **932.8351 Hz, derived exactly** from the spherical Bessel equation | **direct** | **done** — `mep-fem-nafems-test`. Hex20 with **96 elements** gives **+0.05%** |
| FV52 | Simply-supported "solid" square plate | 10 x 10 x 1 m | first 6 frequencies | first mode **45.892 Hz [A, derived]** — Mindlin theory reproduces the remembered 45.897 to four decimals | **direct** | **done** — `mep-fem-nafems-test`. mep meets theory to **0.03%** in the thin limit; at h/a = 0.1 it is 3.5% below Mindlin, which is Mindlin being too stiff |

- [x] **FV52** — simply-supported solid square plate. Written up below.

### FV52: the reference turned out to be derivable, and the disagreement is the theory's

**The value did not have to be taken on trust.** Mindlin thick-plate
theory — Kirchhoff softened by shear deformation and by rotary inertia —
is a closed form, and for this plate it gives **45.8923 Hz**, matching
the figure I remembered to four decimal places. So FV52's reference is
reproducible from theory, and a discrepancy against it becomes a finding
rather than a doubt about the reference. Both theories are computed in
the test.

| | h/a = 0.10 | h/a = 0.05 | h/a = 0.02 |
|---|---|---|---|
| mep | 44.2914 | 23.2309 | 9.4901 |
| Kirchhoff | 47.5345 | 23.7672 | 9.5069 |
| Mindlin | 45.8923 | 23.5537 | 9.4931 |
| mep vs Mindlin | −3.49% | −1.37% | **−0.03%** |

**mep is 3.5% below the published value, and that is the right answer.**
The thickness sweep is what settles it. A uniform error in the mass or
the stiffness would be the same size at every thickness; instead mep
meets theory to three hundredths of a percent where theory is valid, and
parts from it monotonically as the plate thickens. That is Mindlin's own
approximation failing, not mep's: Mindlin keeps a cross-section straight
and corrects the shear with a single factor of 5/6, and a plate a tenth
of its span thick does neither, so Mindlin is too stiff. A
three-dimensional solid model — which is what a benchmark called the
"solid" square plate is asking for — is softer and more accurate.

Two further facts support it. Finite-element frequencies converge **from
above**, and mep's do: 44.6519 on the coarse mesh, 44.2914 on the fine
one. So mep's converged answer is at most 44.29, and no amount of
refinement brings it up to 45.89. And every frequency is below Kirchhoff,
which is what a shear-*locking* element would fail — the Hex8 results
(52.80 falling to 46.49) show that element still on its way down while
Hex20 has arrived.

**The exact degeneracy is the check that owes nothing to any theory.** A
square has a symmetry group, so f(1,2) and f(2,1) are one mode seen
twice. mep gives **107.612264 and 107.612264 Hz, 3.3e-14 apart**. A
solver with an asymmetric mass or stiffness matrix gets the frequencies
roughly right and this exactly wrong.

**Three things I got wrong on the way, all worth keeping.**

*A soft support does not mean a lightly constrained one.* Holding `u_z`
along the mid-plane edges and removing the three in-plane rigid motions
at points looks like the minimal, least-stiffening choice. It is not: the
point constraints remove the rigid motions and leave *nearly*-rigid
flexible ones, which appeared at 37, 60 and 85 Hz — below the plate's own
first bending mode — and filled three of the six slots with in-plane
modes. Holding the mid-plane edges in all three directions removes them
and, as the reasoning predicted, leaves the bending frequency untouched:
44.29 either way. The test now checks that every reported mode is
actually out of plane.

*The mode-shape diagnostic walked the nodes in creation order.* Counting
sign changes along a centre line needs the samples sorted by position,
and the mesh builder creates nodes element by element, so "consecutive"
nodes were not neighbours and the counts were noise. It is also
unreliable for a mode whose nodal line runs along the sampling line —
(2,2) is exactly that case — which is why modes 5 and 6 are reported and
**not** asserted to be a degenerate pair.

*And the first explanation for their 0.13% split was wrong.* A subspace
iteration converges its lowest modes best and its highest worst, so the
last mode of a run is the least trustworthy in it — a good guess, and
testable: asking for ten modes and reading six gives bit-identical
values. The split is in the discrete system, not the eigensolver. Mode 5
is identifiable as (3,1); mode 6 is not (1,3), so the two may not be a
pair at all. Left open below.

- [ ] Identify modes 5 and 6 of FV52 properly, and decide whether their
      0.13% split (0.54% on the coarser mesh, so shrinking) is a
      discretisation-split degeneracy or two genuinely different modes.

- [x] **FV5** — deep simply-supported beam. Written up below.
### FV5: the reference is Timoshenko's, and it is derived

A beam is "deep" when its span is few enough multiples of its depth that
shear deformation and rotary inertia matter, and FV5 exists because that
is where Euler–Bernoulli stops being the answer. So the reference is
**Timoshenko's**, which has a closed form for simply-supported ends.
Assuming `w = W sin(beta x)`, `phi = Phi cos(beta x)` in his coupled pair
reduces them to a quadratic in `omega^2`:

    omega^4 - p omega^2 + q = 0
    p = kappa G beta^2/rho + E beta^2/rho + kappa A G/(rho I)
    q = kappa G E beta^4 / rho^2

whose lower root is the bending mode. As `G` grows without bound the
third term of `p` dominates and `omega^2 -> E I beta^4/(rho A)`, which is
Euler–Bernoulli — so one expression carries both theories and the limit
checks the algebra.

| depth | span/depth | mep | Euler | Timoshenko | vs Euler | vs Timoshenko |
|---|---|---|---|---|---|---|
| 0.50 | 20 | 11.2920 | 11.3362 | 11.2886 | −0.39% | **+0.03%** |
| 1.00 | 10 | 22.3015 | 22.6725 | 22.3001 | −1.64% | **+0.01%** |
| 2.00 | 5 | 42.4294 | 45.3450 | 42.6087 | −6.43% | **−0.42%** |

**mep tracks Timoshenko to hundredths of a percent** where Timoshenko is
good, and falls slightly below it on the deepest beam — the same
over-stiffness a thick-plate theory showed in FV52, and for the same
reason: Timoshenko keeps a cross-section plane and corrects the shear
with a single factor of 5/6, and a beam five times its own depth in span
does neither. The gap to Euler–Bernoulli growing from 0.39% to 6.43% is
the benchmark's whole point made measurable.

**The check that owes nothing to any beam theory is the degeneracy.** A
square section bends alike in `y` and `z`, so the first two modes are one
mode seen twice, and mep gives them identical to better than 1e-8 at
every slenderness. That is a statement about the symmetry of the
assembled matrices, and a support treating the two directions
differently would break it while leaving the frequencies looking
reasonable — which is exactly the trap the support here is built to
avoid.

**Supporting a solid beam means supporting it on its two neutral axes,
one per bending direction.** Holding `u_z` along the line `z = 0`
supports bending in `z` without restraining it, because the neutral axis
of that bending is where the line is. Holding `u_y` along the *same*
line, which is the easy thing to write, restrains the bending it is
meant to support — `u_x` and `u_y` are only zero on the neutral axis of
their own bending.

**And a Hex20 has no node at the centre of a face.** The single axial
constraint goes at the end face's centroid, and with an *odd* number of
elements across the section that point has two odd grid indices — a
face-centre position no element uses — so the lookup returned −1. The
assembly caught it by name ("a constraint names node −1"), which is the
kind of error message worth having.

**One reporting bug of my own**, fixed rather than left: the line
comparing the two theories read `g_depth` *after* the sweep had restored
it, so it printed the 10:1 gap under the 20:1 heading. The theory values
are now captured inside the loop that sets the depth.

- [x] **FV42** — thick hollow sphere, radial modes. Written up below.
### FV42: an exact solution, and finding the right mode among the wrong ones

**The radial modes of a hollow sphere are exactly solvable**, so no
published number is needed. A purely radial displacement reduces the
equations of motion to the spherical Bessel equation of order one,

    U'' + (2/r) U' - 2U/r^2 + k^2 U = 0,    k = omega / c_L

so `U = A j1(kr) + B y1(kr)` with `c_L = sqrt((lambda + 2 mu)/rho)`.
Traction-free at both surfaces gives a two-by-two determinant whose roots
are the frequencies; bisection finds them to machine precision. For
a = 1, b = 2, steel-like properties: **932.8351, 3076.8748, 5886.4809 Hz**.

| mesh | nodes | elements | radial mode | error |
|---|---|---|---|---|
| Hex8 4x4x2 | 183 | 96 | 945.5014 | +1.36% |
| Hex8 8x8x4 | 1085 | 768 | 936.3894 | +0.38% |
| Hex20 2x2x1 | 117 | **12** | 936.9384 | +0.44% |
| Hex20 4x4x2 | 629 | 96 | **933.3030** | **+0.05%** |

Twelve quadratic elements get within half a percent of an exact
three-dimensional elasticity solution, and ninety-six get within five
hundredths. Both sequences converge from above, as a conforming
discretisation must.

**The breathing mode is not the lowest one, and the test finds it by what
it is rather than by where it sits.** An octant with roller faces keeps
every mode symmetric about all three coordinate planes, and the
ellipsoidal ones sit below the radial mode — it came out third of twelve
on every mesh. So each mode is scored by how nearly it moves every point
along its own radius, and the discrimination is sharp rather than lucky:

    radiality of the first four modes: 0.668  0.672  1.000  0.000

A threshold of 0.99 on that has nothing marginal near it. Picking "the
first mode" would have compared an ellipsoidal mode against a radial
reference and reported a 30% error in the solver.

**The mesh is a cubed sphere, not a polar one.** Meshing a sphere in
spherical coordinates puts a degenerate element at every pole — a
hexahedron with an edge collapsed to a point — and those are exactly the
elements a solver will refuse or, worse, accept. Each patch here is the
radial projection of one face of a cube, so every element is well formed,
and the three patches of an octant meet edge to edge. They share their
common nodes through a position-keyed table rather than through index
bookkeeping between patches, and the patch orientations are chosen so
that (u, v, radius) is right-handed on all three — which keeps every
Jacobian positive without any per-element repair afterwards.

**And the octant's three roller planes restrain nothing the mode wants to
do**, which is why an octant is legitimate here at all: a radial mode
moves every point along its own radius, and a radius lies *in* any plane
through the centre.

- [x] **FV22 (the machinery)** — anchored at zero skew, with the
      rhombus's half-turn symmetry verified at every skew. Written up
      below.
- [ ] **FV22 (the benchmark)** — obtain the published frequencies. This
      is the one case in the file where a numerical reference is the only
      kind there is, so there is no substitute for the source.
### FV22: the one case with nothing to derive

Every other benchmark here could be anchored to something computable —
Kirchhoff, Mindlin, Timoshenko, a spherical Bessel determinant, an
exponential decay. **A clamped plate has no exact solution even when it is
rectangular**, and a skewed one has no separable coordinates to try it in.
A numerical reference is the only kind there is, which is precisely what
a benchmark is for, and it is why the published value is genuinely needed
here rather than merely convenient.

What can be done is to anchor the machinery where a tabulated answer
exists — at zero skew, where the rhombus is a square:

    omega_1 a^2 sqrt(rho h / D) = 35.9866   (clamped square)
                                = 19.7392   (simply supported, = 2 pi^2)

The second is exactly derivable and mep already meets it to 0.03% in
FV52's thin limit, so the ratio **1.8231** ties the clamped coefficient
to something this suite has verified independently.

| mesh (h/a = 0.02, no skew) | nodes | mep | vs 35.9866 |
|---|---|---|---|
| 8 x 8 | 837 | 17.9228 | +3.41% |
| 12 x 12 | 1781 | 17.5512 | +1.26% |
| 16 x 16 | 3077 | 17.4378 | **+0.61%** |

**The first mesh read 3.4% high and that is not a failure, it is the
direction a conforming discretisation errs in.** I asserted 2% on one
mesh and it failed; the right question was whether the sequence was
coming *down* to the coefficient, and it is — monotonically, from above.
Worth noting why the same 8x8 mesh that met the simply-supported
coefficient to 0.03% in FV52 is not enough here: **a clamped edge has a
much sharper boundary layer than a supported one**, and resolving it is
what the extra elements buy.

| skew | mode 1 | mode 2 | mode 3 | half-turn symmetry |
|---|---|---|---|---|
| 0 | 80.0662 | 153.4251 | 153.4251 | +1.000, −1.000, −1.000 |
| 30 | 100.2451 | 167.3961 | 209.0296 | +1.000, −1.000, −1.000 |
| 45 | **137.0094** | 208.7782 | 278.3522 | +1.000, −1.000, +1.000 |

**Two checks that owe nothing to any reference.** Every mode of a rhombus
must be symmetric or antisymmetric under the shape's own half-turn, and
mep's are, to better than 1e-6, at every skew — which is a statement
about the geometry and the assembled matrices together, and is what says
the skewed mesh is still the shape it is meant to be. And at zero skew
modes 2 and 3 come out degenerate at 153.4251, the square's (1,2) and
(2,1) pair, which the skewed cases correctly split.

**A skewed plate is stiffer**, and monotonically so: shearing a square
into a rhombus of the same side length shortens one diagonal and reduces
the area, and the fundamental rises 80.07, 100.25, 137.01 Hz. Getting
that backwards would be the signature of a geometry builder that had
sheared the wrong way or scaled instead of shearing.

- [x] **FV32** — cantilevered tapered membrane, as a slab. Written up
      below; it found a real defect in `ModalResult::rigid_body_modes`.
### FV32: a slab has modes the membrane does not, and a rigid-mode count that was wrong

In-plane vibration, so plane stress, so a slab again — with a
complication LE1 did not have. **A slab has out-of-plane modes and a
membrane does not.** The strip is 0.1 m thick and metres deep, so
bending out of its plane is twenty-five times more flexible than bending
within it, and several of the lowest modes of the three-dimensional model
are ones the two-dimensional problem does not possess.

Suppressing them by holding `u_z` everywhere would turn the problem into
plane *strain* and stiffen the answer by about 2.4% — the mistake LE1
exists to warn about. So they are not suppressed: the in-plane modes are
picked out of the list by what they are, exactly as FV42's breathing mode
was. The first in-plane mode came out second of twelve for the slender
strip and **fifth of twelve** for the deep one.

| case | mep | Euler–Bernoulli | |
|---|---|---|---|
| untapered, L/d = 20 | 4.0494 | 4.0385 | **+0.27%** |
| untapered, L/d = 5 | 15.7627 | 16.1540 | −2.42% |
| tapered 2.0 -> 0.5 | **18.9011** | | |

The slender case is the anchor: at twenty to one the shear correction is
under half a percent, so Euler–Bernoulli is nearly the answer and +0.27%
is a real comparison. The deep case falls below it, as FV5 established it
must.

**A tapered cantilever is stiffer than the uniform strip of its root
height, which is the opposite of what I asserted.** I expected the
tapered answer to fall between the two uniform cases it interpolates; it
is above both. On a cantilever the tip carries most of the kinetic energy
and almost none of the strain energy, so material removed there costs
much less than it gains — taper it and the frequency goes *up*, 15.76 to
18.90.

**And it found a real defect in the solver.** `ModalResult::rigid_body_modes`
reported **1** for a strip clamped across its whole root face. The count
compared each eigenvalue against `trace(K)/trace(M)` and called that
scale-free; it is not. That ratio is an average dominated by the
*stiffest* degrees of freedom, and a thin part has a stiffness range of
many orders — so the genuine 1.2 Hz out-of-plane bending mode came in
below the threshold and was labelled rigid. **Telling someone their model
is unrestrained when it is merely flexible is a bad way to be wrong**:
the natural next step is to add constraints that do not belong.

A rigid-body motion is a geometric object, not a small number. The six
are now written out at the nodes, mass-orthonormalised (dropping any the
constraints have removed — a model held on a face has none left, one held
at a point still has three rotations), and each computed mode is counted
as rigid exactly when it lies in their span. For a properly restrained
model every projection is near zero and the count is zero, which is the
right answer reached for the right reason. `mep-fem-modal-test`'s
free-free case still finds its six.

- [x] **FV41** — free cylinder, as a sector. Written up below; the
      reference turned out to need a term the ring formula leaves out.
- [ ] FV2, FV4, FV12, FV15, FV16 — blocked on beams, point masses or shells.

### LE7 and FV41: the sectors, and a reference that was missing a term

Both are axisymmetric, so both are modelled as a **quarter sector** — a
quarter and not some other angle, because the two cut planes must then be
coordinate planes. A sector at any other angle needs `u . n = 0` on a
skew plane, which is a multi-point constraint, and Part I.4 records that
a modal analysis with those is refused by name. The constraints hold
nothing back either way: an axisymmetric response lies in every plane
through the axis.

**LE7.** A thick cylinder under internal pressure has Lamé's exact
solution, and it holds at *every* point rather than asymptotically —
provided the closed ends are represented by the axial traction they
actually exert, `p a^2 / (b^2 - a^2)`, applied uniformly. Do that and
there is no end region to exclude and no excuse for a bad node anywhere.

| mesh | nodes | worst radial | worst hoop | worst axial |
|---|---|---|---|---|
| 2 x 4 x 2 | 141 | 1.61e+06 | 5.03e+05 | 6.33e+05 |
| 4 x 8 x 4 | 785 | 5.33e+05 | 1.94e+05 | 2.16e+05 |
| 6 x 12 x 6 | 2317 | **2.63e+05** | 1.01e+05 | 1.08e+05 |

against a bore hoop stress of 1.67e+07 Pa — so 1.6% at the finest, and
falling. The stress is compared in the *cylindrical* frame, rotating each
recovered tensor onto the local radial and circumferential directions,
which is the only way to compare against Lamé at all.

The cap and the junction are the half of LE7 that remains, and they are
the interesting half: the cylinder's bore hoop stress is 16.67 MPa and a
sphere of the same radii gives 7.14 MPa, so something has to bend where
they meet.

**FV41, and the reference that was wrong.** mep's breathing frequency
came in 0.40% below the thin-ring formula `sqrt(E/rho)/(2 pi R)`, and the
gap *grew* as the wall thinned — 0.06%, 0.33%, 0.40%, 0.44%. That is the
wrong way round for a thick-wall effect, and refining the mesh moved the
answer by four parts in a million, so it was neither.

**The ring formula leaves out the axial motion.** With free ends the hoop
stress does the work alone, but the Poisson contraction is real
displacement carrying real inertia: `u_z(z) = -nu (u/R) z`. Integrating
over a half-length `l` adds `nu^2 l^2 / (3 R^2)` to the effective mass
and nothing to the stiffness, so

    f = f_ring / sqrt(1 + nu^2 l^2 / (3 R^2))

which for this cylinder is 792.807 Hz against the ring's 795.775. That
depends on the cylinder's **length** and not on its wall, which is
exactly why thinning the wall never closed the gap.

| wall | t/R | mep | vs ring | vs cylinder |
|---|---|---|---|---|
| 0.20 | 0.20 | 795.2902 | −0.061% | +0.313% |
| 0.10 | 0.10 | 793.1658 | −0.328% | +0.045% |
| 0.05 | 0.05 | 792.6046 | −0.398% | **−0.026%** |
| 0.02 | 0.02 | 792.2959 | −0.437% | −0.065% |

**And radiality alone could not find the mode, which is the difference
from FV42's sphere.** A short free cylinder has a whole cluster of modes
near the ring frequency, and at a wall of 0.05 the eight lowest scored
0.84, 0.94, 0.92, 0.95, 0.96, 0.99, 0.98, 0.90 for radiality — that
separates nothing. What distinguishes the breathing mode is that it is
*uniform along the axis*, where an axial wave changes sign, and on that
measure the same eight score 0.00, 0.02, 0.00, 0.00, 0.00, **1.00**,
0.02, 0.00. The identification is then not a judgement call.

**FV41 also found a second defect in the rigid-body-mode count**, on top
of the one FV32 found. The fix for FV32 projected each mode onto the six
rigid motions restricted to the free degrees of freedom — but a rigid
motion that a constraint *forbids* must be discarded, not clipped.
Zeroing a translation's components on a symmetry plane leaves a vector
that is no longer a rigid motion at all -- it is a shear -- and counting
projections onto that reported rigid-body modes for a sector that has
none. A motion is now dropped outright if it is nonzero at any
constrained degree of freedom.

---

## T — heat transfer

From *Selected Benchmarks for Heat Transfer Analysis*. mep has steady and
transient conduction with convection and radiation
(`src/fem_thermal.{h,cpp}`), so this series is the best-covered of the
three and the cheapest to land.

| # | name | physics | target | reference | route | notes |
|---|---|---|---|---|---|---|
| T1 | One-dimensional steady conduction with radiation | conduction + radiation boundary | temperature at the radiating face | still **[C]** — see below | **direct** | **done** — `mep-fem-nafems-test`. Asserted against an *exact* solution derived in the test, not against the published number, and it found a bug in the solver's own energy check |
| T2 | One-dimensional heat transfer with radiation | as above, different conditions | temperature at a point | **[C]** | **direct** | |
| T3 | One-dimensional transient heat transfer | transient conduction | temperature at a point and time | still **[C]** — see below | **direct** | **done** — `mep-fem-nafems-test`. Verified against a single-mode exact solution, and the integrator's *order* measured: **1.01** for backward Euler, **2.00** for trapezoidal |
| T4 | Two-dimensional heat transfer with convection | 0.6 x 1.0 m, convective bottom | temperature on the convective face | still **[C]** — the reference is derived instead | slab | **done** — `mep-fem-nafems-test`. Converges at **rate 2.00** on a closed form, and the slab is shown to be **exact to 1e-10** |

- [x] **T1** — 1D steady conduction with radiation. mep reproduces the
      exact conduction-radiation balance to **1.1e-13 K** on every mesh.
      The published value is still unconfirmed and the test does not need
      it. Written up below.
- [ ] **T2** — 1D heat transfer with radiation.
- [x] **T3** — 1D transient. Exact single-mode decay matched, and the
      time integrator's order measured at 1.01 and 2.00. Written up
      below.
### T3: the integrator's order is the part that cannot be faked

The published parameters are not in front of me, so as with T1 the
reference is derived. A slab held at zero on both faces, starting from a
**sinusoidal** distribution, has a single-mode exact solution:

    T(x, t) = T0 sin(pi x / L) exp(-pi^2 alpha t / L^2)

No series, no discontinuity at t = 0 to trip an integrator on, and the
spatial shape is preserved for all time — so a solver that decays by the
right amount while distorting the shape fails a check that a single-point
comparison would pass.

| elements | steps | peak at 100 s | exact | error | energy residual |
|---|---|---|---|---|---|
| 10 | 20 | 33.339832 | 33.650098 | 3.10e-01 | 6.8e-16 |
| 20 | 40 | 33.572488 | 33.650098 | 7.76e-02 | 5.1e-15 |
| 40 | 80 | 33.630693 | 33.650098 | 1.94e-02 | 5.9e-15 |

Refining space and time together gives **rate 2.00**.

**And the orders come out at 1.01 and 2.00.**

| scheme | errors against a 0.031 s reference | measured order |
|---|---|---|
| backward Euler (theta = 1) | 7.79e-01, 3.90e-01, 1.93e-01 | **1.01** |
| trapezoidal (theta = 0.5) | 5.80e-03, 1.45e-03, 3.62e-04 | **2.00** |

This is the check worth having. A value near the right answer says a run
was reasonable; a measured order says the time discretisation is the one
it claims to be. **An integrator with a mistake in its theta weighting
still converges — to the right answer, at the wrong rate** — which is
precisely the failure a single accurate run cannot see. Getting one order
right and the other wrong would be its signature.

Two details that make the measurement mean something. The order is
measured on a **fixed** mesh, so the spatial error is identical in every
run and cancels; and against a **reference run with a far smaller step**
rather than against the analytic answer, which would put a spatial floor
under the comparison and flatten the rate exactly where it matters. In
the accuracy table above, by contrast, space and time are refined
*together*, because halving one alone leaves the other's error dominating
and the sequence stops converging.

The recorded history from Part J.3 is checked at the same time: eleven
asked-for sample times, against the exact decay at each, worst departure
**0.102 K** over a run that falls from 100 K to 33.65 — and monotone,
which this solution is and an unstable step is not.

**T3 passed first time**, which is worth recording precisely because
nothing else in this file did. The transient's own energy check was
already correct — it was the *steady* one that T1 found reading 0.5 —
and the difference between them is instructive: the transient's compares
the heat that crossed the boundary against the heat now stored, which is
a statement with two independently computed sides, while the steady one
had been comparing a quantity against itself.

- [x] **T4** — 2D with convection, as a slab. Written up below.

### T1: a benchmark with no reference value is still a real test

The published number for this one is not in front of me, and this plan's
own rule says not to assert one I cannot check. What rescues it from
being a placeholder is that **the problem has an exact solution that owes
nothing to any of mep's code**. In steady state with constant
conductivity and no source the flux through the bar is uniform, so the
temperature is linear and the radiating face satisfies one scalar
equation:

    k (T_hot - T_face) / L  =  emissivity * sigma * (T_face^4 - T_amb^4)

A dozen lines of Newton solve that to machine precision. It is a
*stronger* reference than a published value at one point, because it
gives the exact temperature at every node and the exact flux everywhere.

Configuration: a 0.1 m bar, k = 55.6 W/(m K), sides insulated, held at
1000 K at one end, radiating to 300 K with emissivity 1 from the other.
Exact face temperature **925.879109 K**, exact flux 41211.2155 W/m2.

| elements | face temperature | error | flux | energy residual | Newton steps |
|---|---|---|---|---|---|
| 4 | 925.879109 | 0.0 | 41211.2155 | 1.1e-15 | 5 |
| 8 | 925.879109 | 0.0 | 41211.2155 | 1.3e-15 | 5 |
| 16 | 925.879109 | 1.1e-13 | 41211.2155 | 3.5e-15 | 5 |

**Exact on every mesh, including the coarsest, and that is the expected
answer rather than a suspicious one.** The exact temperature field is
linear in x, which a trilinear hexahedron represents exactly, so the only
error left is the nonlinear solve's own tolerance. A result that
*converged towards* the answer with refinement would have meant the
radiation boundary was being integrated approximately.

The test checks four things beyond the face value: the whole field
against the exact line, not just the point; that the flux is uniform and
has no transverse component, which is what "one-dimensional and
insulated" means; that Newton actually iterates, since one step would
mean the fourth power had been linearised away; and a **convection
control** with the same bar, whose face temperature is a closed form with
no Newton in it. Without the control, a broken conduction matrix and a
broken radiation term look identical from the outside.

**And it found a bug in the steady solver's own conservation check.**
`ThermalResult::energy_residual` read exactly **5.00e-01** on all three
meshes while the temperatures were right to 1e-13 K. A clean one half is
a normalisation fault, not a physical imbalance. The code summed the
residual *vector* and called it the imbalance — but the residual is zero
at every free node once Newton has converged and carries the reaction at
every fixed one, so its sum is the total heat *supplied*, and dividing
that by a total which counts the supply and the loss alike gives one half
whenever the solve is perfect. It now compares the heat the fixed nodes
supply against the net heat leaving through the surfaces, which is the
statement it always claimed to be making, and reads 1e-15.

A self-check that reports 0.5 for a correct answer is worse than no
self-check: the number looks like a measurement. This is the third time
in this work that an energy or residual check has been normalised by the
wrong quantity — the transient's was, and the nonlinear solver's
convergence test was — and all three had the same shape: a denominator
that was right for the case the author had in mind and meaningless for
the next one.

**Still to do:** confirm the published NAFEMS value and record it here.
The test is written so that adding it is one line, and so that the value
failing to match would be a finding about the configuration rather than
about the solver.

### T4: the slab that costs nothing, and a sign I got backwards

The reference is derived, as for T1 and T3. On a rectangle with `T = 0`
on both sides, a convective face at the bottom and `T = T0 sin(pi x / a)`
across the top, the `x` dependence separates and the `y` part solves
`Y'' = lambda^2 Y` with `lambda = pi/a`, so

    T(x, y) = sin(pi x / a) [A cosh(lambda y) + B sinh(lambda y)]
    B = +(h / (k lambda)) A,   A = T0 / [cosh(lambda b) + (h/(k lambda)) sinh(lambda b)]

One mode, no series, and the convection is *in* the solution rather than
bolted on after it.

| mesh | face temperature | exact | worst nodal error | energy residual |
|---|---|---|---|---|
| 6 x 12 | 0.81283 | 0.89917 | 7.18e-01 | 5.4e-16 |
| 12 x 24 | 0.87719 | 0.89917 | 1.78e-01 | 3.1e-14 |
| 24 x 48 | 0.89365 | 0.89917 | 4.46e-02 | 1.0e-13 |

Rate **2.00**, which is what trilinear elements give on a smooth field.

**I wrote the convection sign the other way round first, and mep
disagreed by 31%.** What identified the fault as mine was not the size of
the disagreement but its behaviour: the worst error went 7.2e-01,
4.3e-01, 4.1e-01 and *stopped*, while the solver's own answer settled
cleanly on 0.90. **A discretisation error shrinks under refinement; a
modelling difference does not**, and a sequence that stalls is pointing
at the model rather than at the mesh. The face at `y = 0` has outward
normal `-y`, so the heat leaving it is `q.n = -k grad T . (-y) = +k
dT/dy`, and convection sets that equal to `h(T - T_ambient)`.

**And the slab costs nothing here, which is the point of including it.**
LE1 had to be extrapolated to zero thickness because a structural slab of
finite thickness is neither plane stress nor plane strain. A thermal slab
has one unknown per node and faces that carry no facet, so they are
insulated, `dT/dz` is zero and the solution has no `z` dependence to
discretise — not approximately, exactly. Asserted rather than assumed:

| variant | face temperature |
|---|---|
| 1 element through, t = 0.10 | 0.877192397489 |
| 3 elements through, t = 0.10 | 0.877192397486 |
| 1 element through, t = **2.00** | 0.877192397485 |
| 4 elements through, t = **0.01** | 0.877192397165 |

A two-hundred-fold change in thickness moves the answer by 3e-10. The
test also checks node for node that nothing varies through the
thickness.

This is also where the energy check that T1 repaired gets exercised on a
problem whose losses are carried by convection rather than radiation: the
heat the fixed edges supply against the heat the convective face removes,
at round-off on every mesh.

Note the asymmetry worth remembering: **a thermal slab is an exact model
of the 2D problem** — one unknown per node, no flux through the faces —
where a structural slab is not. The trap that cost a day in I.5 does not
exist here.

---

## Beyond the three classic series

Named for completeness, not scheduled. Each needs something mep does not
have, and the entry says which.

- **3DNLG series** (geometric nonlinearity, large deflection). mep has
  the total-Lagrangian formulation and arc length (I.6); several of these
  are shells, and the solid ones are candidates.
  - [ ] survey which 3DNLG cases are solid-only.
- **NL series** (material nonlinearity). Blocked on I.7's recorded gap:
  the plastic state is verified as a constitutive routine at a point and
  is not yet carried through the global Newton loop.
- **Contact benchmarks.** Blocked on I.8's gaps — node-to-surface against
  a rigid plane only, no mortar, no friction, no deformable-to-deformable.
- **Buckling.** mep has linear buckling (I.5) and the NAFEMS series for it
  is largely shells.
  - [ ] find a solid buckling case.

---

## Order of work

Chosen so that each one lands against machinery that is already
verified, and so that an early failure is diagnosable:

1. **LE10** — a solid, a pressure, a named stress. If this is wrong,
   everything after it is suspect.
2. **T1** — the simplest thermal, and it brings the radiation
   nonlinearity in early.
3. **FV52** — the first modal, on a solid plate.
4. **LE11** — thermal *stress*, which needs LE10 and T1 to be trusted
   first because it is both.
5. **T3** — the transient, which needs T1's steady answer to be right.
6. **FV5**, **FV42**, **FV22** — more modal, increasingly awkward shapes.
7. **LE1** — the first slab, with the plane-stress error measured.
8. **T4**, **FV32** — the rest of the slabs.
9. **LE7**, **FV41** — the sectors, which need the symmetry constraints
   to be got right and are the most likely to be wrong for a reason that
   has nothing to do with the solver.

---

## The examples, and what they found

The benchmarks also exist as **examples**: `examples/nafems/`, one
`.mepcad` part per case and one Lua script that opens it, states the
benchmark's own conditions, solves, compares against a reference and
writes a film. `just nafems` runs them all; `just nafems-geometry`
rebuilds the parts.

They are not a copy of `src/fem_nafems_test.cpp` and are not meant to
be. That file builds its meshes by hand in C++ and can reach anything
the library can, so it proves the **solver**. The examples use only
`mep.part_*` and `mep.fem_*` -- Part K's thirty-four methods, the same
surface an agent gets over the socket -- so they prove the **surface**.
Where a benchmark cannot be stated through it, the script says so and
stops rather than quietly stating a different problem.

**Five of the nine run. Writing the other four found five defects**, and
that is the part worth recording: the exercise was meant to be a
demonstration and turned into a test.

### What was wrong

**A pressure on a swept mesh acted the wrong way.** `MeshSweep` stored
its profile's rim edges under `Key(u, v)` -- a *sorted* pair, because
that is the key the used-once count is kept under -- and a sorted pair
has lost the one thing the side quadrilateral needs, which is which way
round to be wound. The side faces came out with their normals pointing
in or out according to how the nodes happened to be numbered. Nothing
noticed: `CheckHexes` verifies that the boundary closes, but it does
that from the hexahedra and not from these quadrilaterals.

What noticed was LE7. A pressure acts along the inward normal of the
facet it is on, so a facet wound the wrong way pressurises a cylinder
from the outside: the bore moved *inward* under internal pressure, the
hoop stress came out at a third of Lamé's and with the wrong sign at the
outside, and the model was otherwise perfectly well behaved -- the axial
stress, which came from a traction rather than a pressure, was right to
2%. Fixed; LE7 now matches Lamé through the wall to about a percent.

**The feature tree judged a part in metres by a tolerance meant for
millimetres.** `cad_math.h`'s `Tolerance` says in as many words that its
1e-7 default is "the value most kernels settle on for models measured in
millimetres", and that one global epsilon is the classic way a kernel
that works on its own primitives falls over. An extruded ellipse 3.25 m
across follows its own edge to about a micron -- three parts in ten
million, and entirely respectable -- and failed validation outright, so
LE1 and LE10 would not build at all. The part was not wrong; the
yardstick was. `cad_feature.cpp` now scales the tolerance to the body's
own diagonal, with the old absolute value as a floor.

**`part.faces` reported a point that is not on the face.** `point` is
the *surface* evaluated at the middle of its own parameter domain, and a
face is a trimmed piece of a surface: LE10's outer elliptical face
reports a point at (-3.25, 0, 0), on the opposite side of the whole
ellipse and outside the part. A script picking "the face nearest
(3.25, 0, 0)" therefore gets the face bounding the *hole*, the plate is
built in along the wrong edge, and the answer is wrong by a factor of
three with nothing having failed. `part.faces` now also reports a
`centroid` and a bounding box taken from the face's own tessellation,
which honour the trimming by construction.

**The stress components had no compact field names.** `stress_yy` is the
*target quantity* of two of these benchmarks and came back as "no field
called 'stress_yy'"; only the display names, which have spaces in them,
were accepted. Aliases added for all six components and the three
displacement components.

**`ExtrudeEnd::Symmetric` was documented backwards.** The header said
"`distance` each way from the sketch plane"; the rebuild treats it as
the total. Every part built from that comment came out half the size it
was asked for. Comment fixed.

### What is still wrong

**The surface mesher fails on a planar face that is not a rectangle.**
A rhombus and a trapezoid are both enough; a square and a rectangle are
fine. `MeshSurface` returns a triangulation that is not closed, so the
tetrahedral mesher has no volume to fill and the sweeper's quadrangulated
profile contains an inverted element. It fails at *every* element size
tried, from 2.5 m down to 0.1 m, so it is structural rather than a
sliver.

This blocks **FV22** (a 45-degree rhombic plate) and **FV32** (a tapered
strip) outright, and both bodies pass `part.validate` cleanly. It is the
largest single defect this exercise found and the one most likely to
matter elsewhere -- a tapered or skewed prism is not an exotic part.

**The tetrahedral mesher does not terminate on a thin slab.** LE1's part
is 0.05 m thick and 6.5 m across; `fem.mesh` runs without returning. The
sweeper does return, with a boundary that does not close. **LE1** is
blocked.

**A hollow sphere cannot be made through the surface at all.** Three
routes, three refusals, each a documented limitation rather than a bug:
a revolve refuses a profile that touches its axis, which a sphere's
necessarily does at both poles; `cad_boolean` refuses to cut a face whose
parameterisation has a pole; and `part.shell` handles planar faces only.
**FV42** is blocked -- though `examples/nafems/fv42.lua` still derives
the exact reference from the traction-free determinant, because that part
owes nothing to the kernel, and `src/fem_nafems_test.cpp` does run FV42
on a cubed-sphere mesh built in C++.

**The four thermal benchmarks are not examples at all.** T1, T3, T4 and
LE11 all need a thermal study, and there is no thermal binding:
`fem_thermal.cpp` implements conduction, convection, radiation and the
transient integrator, and none of it is on the agent surface. That is a
missing binding rather than a missing capability, and it is the largest
gap in Part K.

### What the examples needed that did not exist

Three things, each now on the surface:

- **`fem.mesh method="sweep"`.** Part G.6's hexahedral sweeper was built
  for exactly the parts these benchmarks are -- "Hex8 behaves far better
  than Tet4 under bending, and a prismatic part is exactly the sort that
  gets bent" -- and was reachable only from C++. Five of the nine
  examples use it, and LE10 runs *both* meshers, which is a stronger
  statement than either: a linear hexahedron and a quadratic tetrahedron
  land on opposite sides of the published -5.38 MPa and bracket it.
- **`part.edges`, `part.edge_at`, and edge supports.** A simple support
  is a *line*. Expressed as a face it becomes a built-in end, which is a
  different structure; FV5 and FV52 are both simply supported and neither
  could be stated. `fem::EdgeTarget` already existed; nothing could name
  an edge to hand to it.
- **`fem.nodes`.** A mode identified by what it *is* rather than by where
  it sits in the list needs the shape and the node coordinates in the
  same indexing, and the surface handed out the first and kept the
  second. FV41's breathing mode, FV5's vertical bending modes and FV32's
  in-plane modes are all picked this way.

Plus `mep --run-lua`, so a script that drives this surface can be run
from a Makefile or a machine with no display -- and `fem.movie` and
`fem.image`, which are Part J.3's other half and have their own section
in `plans/CAD_FEM_PLAN.md`.

---

## Reference values

Every `[B]` and `[C]` above needs its number before the test can assert
it. They are published in the NAFEMS volumes and reproduced in the
verification manuals of most FE packages; the ones in the tree already —
CalculiX, and Gmsh for meshes — are the obvious cross-check, and where a
value cannot be confirmed the benchmark can still be run against
CalculiX on the identical mesh, which is a weaker claim honestly stated
rather than a stronger one guessed at.

- [ ] Confirm every `[B]` value.
- [ ] Obtain every `[C]` value.
- [ ] Record the source for each in this file next to the value, so the
      next reader can check it without repeating the search.
