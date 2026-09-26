-- NAFEMS FV32 -- cantilevered tapered membrane, in-plane vibration.
--
-- A strip 10 m long, 2 m deep at the root and 0.5 m at the tip, 0.1 m
-- thick, clamped at the wide end. E = 200 GPa, v = 0.3,
-- rho = 8000 kg/m3. The benchmark is about the *in-plane* modes.
--
-- A SLAB HAS OUT-OF-PLANE MODES AND A MEMBRANE DOES NOT. The strip is
-- 0.1 m thick and metres deep, so bending out of its own plane is
-- twenty-five times more flexible than bending within it, and the lowest
-- several modes of the three-dimensional model are ones the
-- two-dimensional problem does not possess at all. Suppressing them by
-- holding u_z everywhere would turn the problem into plane *strain* and
-- stiffen the in-plane answer by about 2.4% -- the mistake LE1 exists to
-- warn about. So they are not suppressed: the in-plane modes are picked
-- out of the list by what they are.
--
-- The anchor is the untapered case, whose in-plane bending is an
-- Euler-Bernoulli cantilever and has a coefficient. A taper does not
-- lower the frequency, as most people guess -- it raises it, because the
-- tip that was removed carried far more kinetic energy than stiffness.

local nafems = dofile((debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or ".") .. "/nafems.lua")

nafems.heading("NAFEMS FV32 -- cantilevered tapered membrane")

local length, thickness = 10.0, 0.1
local root, tip = 2.0, 0.5
local modulus, poisson, density = 200e9, 0.3, 8000.0

-- The untapered anchor: the first root of cos(bL) cosh(bL) + 1 = 0 is
-- 1.8751041, and a cantilever's fundamental follows from it.
local function cantilever(height)
    local area = height * thickness
    local inertia = thickness * height ^ 3 / 12.0
    local beta = 1.87510407
    return beta * beta * math.sqrt(modulus * inertia / (density * area * length ^ 4))
           / (2.0 * nafems.pi)
end
nafems.note(string.format("an untapered strip %.1f m deep would have its first in-plane bending",
                          root))
nafems.note(string.format("mode at %.4f Hz, and one %.2f m deep at %.4f Hz; the taper's answer",
                          cantilever(root), tip, cantilever(tip)))
nafems.note("is above both, because the tip carried kinetic energy and little stiffness.")

local document, body = nafems.open("fv32")
local mesh = nafems.mesh(document, body, {0.15, 0.25, 0.4}, 1, {"sweep", "tetrahedra"})
if not mesh then
    os.exit(nafems.blocked("FV32", "the mesher will not take a tapered planar face"))
end

local study = mep.fem_study{document = document, name = "FV32"}.study
mep.fem_material{study = study, youngs_modulus = modulus, poissons_ratio = poisson,
                 density = density}
-- Clamped at the root, and nowhere else.
local clamped = nafems.one_face(document, body, function(face)
    return math.abs(face.low[1]) < 1e-6 and math.abs(face.high[1]) < 1e-6
end, "the root")
mep.fem_support{study = study, face = clamped, label = "clamped root"}

local modal = mep.fem_modal{study = study, mesh = mesh.mesh, modes = 12}
nafems.note(string.format("%d modes, of which %d are rigid-body motions (a clamped part has none)",
                          #modal.frequencies, modal.rigid_body_modes))

-- In-plane: the motion is in x and y, not z.
local in_plane = {}
local coordinates = mep.fem_nodes{result = modal.result}
for mode = 0, #modal.frequencies - 1 do
    local shape = mep.fem_animate{result = modal.result, mode = mode, frames = 1}.displacement[1]
    local within, out = 0.0, 0.0
    for n = 1, coordinates.count do
        within = within + shape[(n - 1) * 3 + 1] ^ 2 + shape[(n - 1) * 3 + 2] ^ 2
        out = out + shape[(n - 1) * 3 + 3] ^ 2
    end
    local fraction = within / math.max(1e-30, within + out)
    if fraction > 0.9 then
        in_plane[#in_plane + 1] = {mode = mode, frequency = modal.frequencies[mode + 1],
                                   fraction = fraction}
    end
end
nafems.note(string.format("%d of the %d modes are in-plane; the rest are the slab bending out",
                          #in_plane, #modal.frequencies))

if #in_plane == 0 then
    nafems.note("no in-plane mode is in the first dozen; ask for more")
else
    local first = in_plane[1]
    nafems.note(string.format("the first in-plane mode is number %d at %.4f Hz (%.1f%% of its"
                              .. " motion is in-plane)", first.mode + 1, first.frequency,
                              100.0 * first.fraction))
    nafems.checks = nafems.checks + 1
    if first.frequency > cantilever(root) and first.frequency > cantilever(tip) then
        nafems.note("it is above both untapered strips, as a taper must be")
    else
        nafems.failures = nafems.failures + 1
        nafems.note("it is NOT above both untapered strips, which a taper has to be")
    end
    nafems.still(modal.result, "fv32", {mode = first.mode, caption = "FV32  FIRST IN-PLANE MODE",
                                        yaw = 60, pitch = 60})
    nafems.film(modal.result, "fv32", {mode = first.mode, frames = 30,
                                       caption = "FV32  TAPERED MEMBRANE", yaw = 60, pitch = 60})
end

os.exit(nafems.verdict("FV32"))
