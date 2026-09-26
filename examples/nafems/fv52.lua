-- NAFEMS FV52 -- simply supported "solid" square plate, free vibration.
--
-- A 10 x 10 x 1 m plate, E = 200 GPa, v = 0.3, rho = 8000 kg/m3, simply
-- supported along all four lower edges. Named "solid" in the source
-- because it is meant for solid elements: at a tenth of its span in
-- thickness it is too thick for Kirchhoff theory, which is what makes it
-- a benchmark rather than an exercise.
--
-- THE REFERENCE IS DERIVED, NOT RECALLED. Mindlin's thick-plate theory
-- has a closed form for every mode of a simply-supported rectangle:
-- Kirchhoff softened by shear deformation and by rotary inertia, the two
-- things a thin-plate theory leaves out and a plate this thick very much
-- has. It is not exact for a three-dimensional solid, but it is close,
-- and it predicts the *direction* of the residual error: a real plate is
-- softer still, so every frequency should come in at or below it. The
-- commonest failure of a low-order solid element here is shear locking,
-- which makes it too stiff and would put it above.
--
-- A SIMPLE SUPPORT IS A LINE, and that is why part.edges and edge
-- supports exist at all. Held as a *face* the plate is built in, which
-- is a different structure with frequencies about half again as high.
-- Held only in z, the plate is still free to slide in its own plane and
-- to spin about the vertical -- three rigid-body motions, which is
-- correct: a simple support carries no in-plane load. They are not
-- suppressed with extra constraints, because any constraint that removed
-- them would also stiffen the plate in its own plane. fem.modal counts
-- them instead, and the bending modes are the ones after.

local nafems = dofile((debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or ".") .. "/nafems.lua")

nafems.heading("NAFEMS FV52 -- simply supported square plate")

local side, thickness = 10.0, 1.0
local modulus, poisson, density = 200e9, 0.3, 8000.0

local document, body = nafems.open("fv52")
local mesh = nafems.mesh(document, body, {1.0, 1.25, 1.6}, 1, {"sweep", "tetrahedra"})
if not mesh then os.exit(nafems.blocked("FV52", "no mesher would take the plate")) end

local study = mep.fem_study{document = document, name = "FV52"}.study
mep.fem_material{study = study, youngs_modulus = modulus, poissons_ratio = poisson,
                 density = density}

-- The four lower edges, each held in z alone.
for _, where in ipairs{{{5, 0, 0}, {1, 0, 0}}, {{5, side, 0}, {1, 0, 0}},
                       {{0, 5, 0}, {0, 1, 0}}, {{side, 5, 0}, {0, 1, 0}}} do
    local edge, found = nafems.edge_near(document, body, where[1], where[2])
    mep.fem_support{study = study, edge = edge, fixed = {false, false, true},
                    label = "simple support"}
    nafems.note(string.format("simple support on the edge from (%.1f, %.1f, %.1f) to"
                              .. " (%.1f, %.1f, %.1f), %.2f m long",
                              found.from[1], found.from[2], found.from[3],
                              found.to[1], found.to[2], found.to[3], found.length))
end

local modal = mep.fem_modal{study = study, mesh = mesh.mesh, modes = 8}
nafems.note(string.format("%d modes, of which %d are rigid-body motions",
                          #modal.frequencies, modal.rigid_body_modes))

-- Mindlin's own answers, and mep's, side by side. The (1,2) and (2,1)
-- modes of a square are exactly degenerate, so they arrive as a pair --
-- a solver that got the frequencies roughly right and the degeneracy
-- wrong would have an asymmetric mass or stiffness matrix, and no single
-- frequency would show it.
local wanted = {{1, 1}, {1, 2}, {2, 1}, {2, 2}}
local rigid = modal.rigid_body_modes
for index, mn in ipairs(wanted) do
    local reference = nafems.mindlin(mn[1], mn[2], side, thickness, modulus, poisson, density)
    local got = modal.frequencies[rigid + index]
    if got then
        nafems.compare(string.format("mode (%d,%d)", mn[1], mn[2]), got, reference, "Hz", 0.06)
    end
end

nafems.checks = nafems.checks + 1
local pair = math.abs(modal.frequencies[rigid + 2] - modal.frequencies[rigid + 3])
if pair < 0.02 * modal.frequencies[rigid + 2] then
    nafems.note(string.format("(1,2) and (2,1) are degenerate to %.4f%%, as a square's must be",
                              100.0 * pair / modal.frequencies[rigid + 2]))
else
    nafems.failures = nafems.failures + 1
    nafems.note("(1,2) and (2,1) are NOT degenerate; the matrices are not symmetric in x and y")
end

nafems.still(modal.result, "fv52", {mode = rigid, caption = "FV52  FIRST BENDING MODE",
                                    yaw = 40, pitch = 35})
nafems.film(modal.result, "fv52", {mode = rigid, frames = 30,
                                   caption = "FV52  SIMPLY SUPPORTED PLATE",
                                   yaw = 40, pitch = 35})

os.exit(nafems.verdict("FV52"))
