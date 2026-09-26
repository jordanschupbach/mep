-- NAFEMS LE1 -- elliptic membrane, plane stress.
--
-- The same quarter ellipse-with-a-hole as LE10, but thin and loaded by
-- an outward pressure of 10 MPa on its outer edge rather than on its
-- face. The target is the direct stress sigma_yy at A -- the hole, on
-- the x axis -- published as 92.7 MPa.
--
-- MEP HAS NO PLANE-STRESS SOLVE PATH. The assembly is three degrees of
-- freedom per node throughout, so this runs as a three-dimensional slab,
-- and plane stress is a *limit* rather than an identity: a slab of
-- finite thickness is neither plane stress nor plane strain, and
-- approaches the first only as the thickness goes to zero.
--
-- The two mistakes available here are opposite and both easy. Holding
-- u_z on the *faces* makes it plane strain, which is a different problem
-- -- Part I.5 lost a day to exactly that. Holding u_z nowhere leaves the
-- slab free to drift. The mid-plane is the answer: a plane-stress state
-- has u_z proportional to z, so it is already zero there, and holding it
-- there removes the rigid motion while restraining nothing. That is why
-- the part is *half* a 0.1 m slab, from the mid-plane up: the mid-plane
-- is then a face, and a face is what a support can be put on.

local nafems = dofile((debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or ".") .. "/nafems.lua")

nafems.heading("NAFEMS LE1 -- elliptic membrane, quarter of half a slab")

local document, body = nafems.open("le1")
nafems.note(string.format("the modelled eighth is %.5f m3",
                          mep.part_mass{document = document, body = body}.volume))

-- THE MESHER WILL NOT TAKE THIS PART, and the script says so rather
-- than hanging or pretending. A 0.05 m slab 6.5 m across is the case
-- mep's surface mesher is worst at: the tetrahedral path runs without
-- terminating and the sweeper reports a boundary that does not close.
-- Everything below this line is the benchmark stated correctly and
-- waiting for a mesh. See plans/NAFEMS_PLAN.md.
local mesh = nafems.mesh(document, body, {0.20, 0.30, 0.50}, 1, {"sweep"})
if not mesh then
    os.exit(nafems.blocked("LE1", "the mesher will not take a 0.05 m slab 6.5 m across"))
end

local study = mep.fem_study{document = document, name = "LE1"}.study
mep.fem_material{study = study, youngs_modulus = 210e9, poissons_ratio = 0.3, density = 7800}

nafems.symmetry(study, document, body, "x", 0.0)
nafems.symmetry(study, document, body, "y", 0.0)
-- The mid-plane. Not the faces: that is plane strain.
nafems.symmetry(study, document, body, "z", 0.0)

local outer = nafems.one_face(document, body, function(face)
    return math.abs(face.high[1] - 3.25) < 1e-3 and math.abs(face.high[2] - 2.75) < 1e-3
           and face.high[3] > face.low[3] + 1e-9
end, "the outer elliptical edge")
-- NEGATIVE, because fem.load's pressure is positive *into* the surface
-- and this one pulls outward. A benchmark whose sign is wrong still
-- solves, still converges, and answers the compression problem.
mep.fem_load{study = study, kind = "pressure", face = outer, magnitude = -10e6,
             label = "10 MPa outward"}

local result = mep.fem_solve{study = study, mesh = mesh.mesh}
nafems.note(string.format("solved: %d nodes, peak movement %.4g m, estimated error %.2f%%",
                          result.nodes, result.max_displacement, result.relative_error * 100.0))

local probe = mep.fem_probe{result = result.result, field = "stress_yy",
                            points = {{2.0, 0.0, 0.0}}}
nafems.compare("sigma_yy at A, mid-plane", probe.probes[1].value / 1e6, 92.7, "MPa", 0.10)

-- The slab's own error, stated rather than absorbed. A 0.1 m slab is
-- about a percent stiffer in this measure than the zero-thickness limit,
-- and src/fem_nafems_test.cpp gets at the limit by sweeping the
-- thickness and extrapolating. The example does not sweep -- it is one
-- part with one thickness -- so the residue is named instead of being
-- reported as the solver's.
nafems.note("a 0.1 m slab is not plane stress; the unit test sweeps the thickness and")
nafems.note("extrapolates to zero, and lands within 0.5% of the published 92.7 MPa.")

-- Along the hole, from the x axis round to the y axis: the hoop stress
-- a membrane with an elliptical hole carries, which is the shape of the
-- answer rather than one number from it.
local around = mep.fem_path{result = result.result, field = "stress_yy",
                            from = {2.0, 0.0, 0.0}, to = {0.0, 1.0, 0.0}, samples = 5}
nafems.note("sigma_yy from A round toward B (a straight chord, not the arc):")
for _, row in ipairs(around.rows) do
    nafems.note(string.format("    (%5.2f, %5.2f)   %9.4g MPa", row.at[1], row.at[2],
                              row.value / 1e6))
end

nafems.still(result.result, "le1", {field = "stress_yy", caption = "LE1  SIGMA YY",
                                    yaw = 40, pitch = 55, color_map = "blue_to_red"})
nafems.film(result.result, "le1", {field = "max_principal", frames = 36, orbit = true,
                                   caption = "LE1  ELLIPTIC MEMBRANE", pitch = 45})

os.exit(nafems.verdict("LE1"))
