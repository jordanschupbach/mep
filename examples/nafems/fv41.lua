-- NAFEMS FV41 -- free hollow cylinder, axisymmetric vibration.
--
-- Mid-surface radius 1 m, wall 100 mm, length 1 m, free at both ends.
-- E = 200 GPa, v = 0.3, rho = 8000 kg/m3. A quarter sector is modelled
-- with symmetry on the two cut planes, which costs nothing: an
-- axisymmetric response lies in every plane through the axis.
--
-- THE RING FORMULA IS NOT THE REFERENCE, and finding that out is most of
-- what this benchmark taught. A thin ring breathing radially has
--
--     f_ring = sqrt(E / rho) / (2 pi R) = 795.77 Hz
--
-- and mep came in 0.4% below it, with the gap *growing* as the wall was
-- thinned -- the wrong way round for a thick-wall effect -- while
-- refining the mesh moved the answer by four parts in a million. So it
-- was neither the wall nor the mesh.
--
-- What the ring formula leaves out is the axial motion. With free ends
-- the hoop stress does the work alone, but the Poisson contraction is
-- real displacement carrying real inertia: u_z = -v (u/R) z. Integrating
-- over a half-length l adds v^2 l^2 / (3 R^2) to the effective mass and
-- nothing to the stiffness, so
--
--     f = f_ring / sqrt(1 + v^2 l^2 / (3 R^2))
--
-- which depends on the cylinder's *length* and not on its wall -- which
-- is exactly why thinning the wall never closed the gap.
--
-- AND RADIALITY ALONE CANNOT FIND THE MODE. A short free cylinder has a
-- whole cluster of modes near the ring frequency; at a thin wall the
-- eight lowest all score between 0.84 and 0.99 for how radial they are,
-- which separates nothing. What distinguishes the breathing mode is that it is
-- *uniform along the axis*, where an axial wave changes sign, and on
-- that measure the same eight score 0.00 to 0.02 against its 1.00. The
-- identification below is that measure, not a judgement call.

local nafems = dofile((debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or ".") .. "/nafems.lua")

nafems.heading("NAFEMS FV41 -- free hollow cylinder, quarter sector")

local radius, length = 1.0, 1.0
local modulus, poisson, density = 200e9, 0.3, 8000.0
local ring = math.sqrt(modulus / density) / (2.0 * nafems.pi * radius)
local half = length * 0.5
local cylinder = ring / math.sqrt(1.0 + poisson * poisson * half * half / (3.0 * radius * radius))
nafems.note(string.format("a ring would give %.4f Hz; a cylinder of this length, carrying the",
                          ring))
nafems.note(string.format("inertia of its own Poisson contraction, gives %.4f Hz", cylinder))

local document, body = nafems.open("fv41")
local mesh = nafems.mesh(document, body, {0.15, 0.20, 0.25}, 1, {"sweep", "tetrahedra"})
if not mesh then os.exit(nafems.blocked("FV41", "no mesher would take the quarter tube")) end

local study = mep.fem_study{document = document, name = "FV41"}.study
mep.fem_material{study = study, youngs_modulus = modulus, poissons_ratio = poisson,
                 density = density}
nafems.symmetry(study, document, body, "x", 0.0)
nafems.symmetry(study, document, body, "y", 0.0)
-- Nothing holds it axially. It is a free cylinder and the axial motion
-- is half the physics.

local modal = mep.fem_modal{study = study, mesh = mesh.mesh, modes = 12}
nafems.note(string.format("%d modes, of which %d are rigid-body motions",
                          #modal.frequencies, modal.rigid_body_modes))

-- Radial, and uniform along the axis. The second factor is what picks
-- the breathing mode out of the cluster: (sum u_r)^2 / (N sum u_r^2) is
-- one when every station moves the same way and nearly nothing when the
-- sign alternates along the length.
local breathing, score, all = nafems.pick_mode(modal.result, #modal.frequencies, function(nodes)
    local radial_energy, total, sum, square = 0.0, 0.0, 0.0, 0.0
    for _, node in ipairs(nodes) do
        local r = math.sqrt(node.x * node.x + node.y * node.y)
        local u_r = r > 1e-9 and (node.ux * node.x + node.uy * node.y) / r or 0.0
        radial_energy = radial_energy + u_r * u_r
        total = total + node.ux ^ 2 + node.uy ^ 2 + node.uz ^ 2
        sum = sum + u_r
        square = square + u_r * u_r
    end
    if total <= 0.0 or square <= 0.0 then return 0.0 end
    local radial = radial_energy / total
    local uniform = sum * sum / (#nodes * square)
    return radial * uniform
end)

nafems.note("mode / frequency / radial x axially-uniform:")
for mode = 0, #modal.frequencies - 1 do
    nafems.note(string.format("    %2d  %9.3f Hz   %.3f%s", mode + 1, modal.frequencies[mode + 1],
                              all[mode + 1], mode == breathing and "   <- breathing" or ""))
end

nafems.compare("the breathing mode", modal.frequencies[breathing + 1], cylinder, "Hz", 0.02)
local runner_up = 0.0
for mode = 0, #modal.frequencies - 1 do
    if mode ~= breathing then runner_up = math.max(runner_up, all[mode + 1]) end
end
nafems.note(string.format("it scores %.3f on radial-and-axially-uniform; the next best scores"
                          .. " %.3f, so the identification is not a judgement call",
                          score, runner_up))
nafems.note(string.format("against the thin-ring formula it is %+.3f%% -- which is the"
                          .. " correction, not an error",
                          100.0 * (modal.frequencies[breathing + 1] / ring - 1.0)))

nafems.still(modal.result, "fv41", {mode = breathing, caption = "FV41  BREATHING MODE",
                                    yaw = 35, pitch = 30})
nafems.film(modal.result, "fv41", {mode = breathing, frames = 30,
                                   caption = "FV41  FREE HOLLOW CYLINDER",
                                   yaw = 35, pitch = 30})

os.exit(nafems.verdict("FV41"))
