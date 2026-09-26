-- NAFEMS LE7 -- thick cylinder under internal pressure.
--
-- Bore 1 m, outside 2 m, 10 MPa on the bore, closed ends. A quarter
-- sector is modelled, and only half the length: the mid-length plane at
-- z = 0 is a symmetry plane, and the closed end's axial pull is applied
-- as a traction on the face at z = 1.
--
-- THE REFERENCE IS EXACT EVERYWHERE, which is what makes this the most
-- searching static case in the set. Lame's solution for a thick cylinder
-- is closed form at every point in the wall, not an asymptotic one, so
-- there is no "away from the ends" region to hide in -- provided the
-- closed end really is represented by the traction it exerts,
-- p a^2 / (b^2 - a^2), rather than being left off. Leave it off and the
-- axial stress is zero, the hoop stress is still nearly right, and the
-- model looks fine.
--
-- The benchmark's own part is this cylinder capped by a hemisphere. This
-- is the cylinder. The cap and the junction -- where a 16.67 MPa bore
-- hoop stress has to meet a sphere's 7.14 MPa, so something bends -- are
-- the half that is still open, in plans/NAFEMS_PLAN.md.

local nafems = dofile((debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or ".") .. "/nafems.lua")

nafems.heading("NAFEMS LE7 -- thick cylinder, quarter sector, half length")

local a, b, pressure = 1.0, 2.0, 10e6
local document, body = nafems.open("le7")
nafems.note(string.format("the modelled quarter is %.4f m3", 
                          mep.part_mass{document = document, body = body}.volume))

local mesh = nafems.mesh(document, body, {0.15, 0.20, 0.25, 0.35}, 1, {"sweep", "tetrahedra"})
if not mesh then os.exit(nafems.blocked("LE7", "no mesher would take the quarter tube")) end

local study = mep.fem_study{document = document, name = "LE7"}.study
mep.fem_material{study = study, youngs_modulus = 210e9, poissons_ratio = 0.3, density = 7800}

nafems.symmetry(study, document, body, "x", 0.0)
nafems.symmetry(study, document, body, "y", 0.0)
nafems.symmetry(study, document, body, "z", 0.0)   -- the mid-length plane

local spans_length = function(face) return face.high[3] > face.low[3] + 1e-9 end
local bore = nafems.one_face(document, body, function(face)
    return spans_length(face) and math.abs(face.high[1] - a) < 1e-3
           and math.abs(face.high[2] - a) < 1e-3
end, "the bore")
mep.fem_load{study = study, kind = "pressure", face = bore, magnitude = pressure,
             label = "10 MPa on the bore"}

-- The closed end, as the traction it really exerts. Without this the
-- cylinder is open-ended and Lame's axial term is not zero but the
-- model's is.
local axial = pressure * a * a / (b * b - a * a)
local endcap = nafems.one_face(document, body, function(face)
    return math.abs(face.low[3] - 1.0) < 1e-6 and math.abs(face.high[3] - 1.0) < 1e-6
end, "the end at z = 1")
mep.fem_load{study = study, kind = "traction", face = endcap, vector = {0, 0, axial},
             label = "closed end"}
nafems.note(string.format("the closed end pulls with %.4f MPa of axial traction",
                          axial / 1e6))

local result = mep.fem_solve{study = study, mesh = mesh.mesh}
nafems.note(string.format("solved: %d nodes, peak movement %.4g m, estimated error %.1f%%",
                          result.nodes, result.max_displacement, result.relative_error * 100.0))

-- Compared on the x axis, where the cylindrical frame and the Cartesian
-- one coincide: there the radial stress is sigma_xx and the hoop stress
-- is sigma_yy. Away from the axis they would have to be rotated, which
-- the unit test does and an example does not need to.
nafems.note("on the x axis at mid-length, where radial is xx and hoop is yy:")
-- SAMPLED AWAY FROM THE SURFACES, deliberately. A probe a hundredth of
-- a metre from the bore sits inside the first element, where a recovered
-- stress is at its least reliable -- and Lame's radial stress at the
-- outside is *zero*, so a percentage against it means nothing whatever
-- the model does. The interior is where the closed form and the solution
-- can be compared on equal terms; the boundary values are reported
-- below on their own footing.
for _, r in ipairs{1.15, 1.35, 1.55, 1.75, 1.9} do
    local exact = nafems.lame(pressure, a, b, r)
    local radial = mep.fem_probe{result = result.result, field = "stress_xx",
                                 points = {{r, 0.0, 0.0}}}
    local hoop = mep.fem_probe{result = result.result, field = "stress_yy",
                               points = {{r, 0.0, 0.0}}}
    nafems.close(string.format("r = %.2f  radial", r), radial.probes[1].value / 1e6,
                 exact.radial / 1e6, "MPa", 0.5)
    nafems.compare(string.format("r = %.2f  hoop", r), hoop.probes[1].value / 1e6,
                   exact.hoop / 1e6, "MPa", 0.05)
end
local axial_probe = mep.fem_probe{result = result.result, field = "stress_zz",
                                  points = {{1.5, 0.0, 0.0}}}
nafems.compare("axial, anywhere in the wall", axial_probe.probes[1].value / 1e6, axial / 1e6,
               "MPa", 0.06)

-- The two boundary conditions the solution has to satisfy exactly, which
-- is a different claim from the interior agreement above: the radial
-- stress is minus the pressure at the bore and nothing at the outside.
-- Both are traction conditions the solve was never told about directly
-- -- it was given a pressure, not a stress -- so meeting them is the
-- model checking its own arithmetic.
for _, edge in ipairs{{1.005, -pressure / 1e6, "at the bore"},
                      {1.995, 0.0, "at the outside"}} do
    local probe = mep.fem_probe{result = result.result, field = "stress_xx",
                                points = {{edge[1], 0.0, 0.0}}}
    nafems.close("radial " .. edge[3], probe.probes[1].value / 1e6, edge[2], "MPa", 1.8)
end

nafems.still(result.result, "le7", {field = "von_mises", caption = "LE7  VON MISES",
                                    yaw = 30, pitch = 35})
nafems.film(result.result, "le7", {field = "von_mises", frames = 36, orbit = true,
                                   caption = "LE7  THICK CYLINDER", pitch = 25})

os.exit(nafems.verdict("LE7"))
