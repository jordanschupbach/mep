-- NAFEMS FV22 -- clamped thick rhombic plate, free vibration.
--
-- Sides of 10 m skewed 45 degrees, 1 m thick, clamped on all four side
-- faces. E = 200 GPa, v = 0.3, rho = 8000 kg/m3.
--
-- THIS ONE HAS NO CLOSED FORM, and that is exactly why it is a
-- benchmark. Every other case in this set can be anchored to something
-- derivable -- Kirchhoff, Mindlin, Timoshenko, Lame, a ring with its
-- Poisson correction -- but a *clamped* plate has no exact solution even
-- when it is rectangular, and a skewed one has no separable coordinates
-- to try it in. A numerical reference is the only kind there is.
--
-- So the machinery is anchored at zero skew instead, where the clamped
-- square plate has a coefficient tabulated to five figures:
--
--     omega_1 a^2 sqrt(rho h / D) = 35.9866   (clamped square)
--                                 = 19.7392   (simply supported, = 2 pi^2)
--
-- The second is exactly derivable and FV52 already meets it in the thin
-- limit, so their ratio -- 1.8231 -- ties the clamped coefficient to
-- something this set has verified. The skewed answer is then reported
-- against the square, where the claim that can be made is a direction:
-- skewing a clamped plate stiffens it, so the frequency must rise.
--
-- A CLAMPED PLATE IS THE ONE SUPPORT CONDITION THAT NEEDS NO EDGE. All
-- four sides are held entirely, and a side of a solid plate is a face.

local nafems = dofile((debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or ".") .. "/nafems.lua")

nafems.heading("NAFEMS FV22 -- clamped rhombic plate")

local side, thickness = 10.0, 1.0
local modulus, poisson, density = 200e9, 0.3, 8000.0

local function thin_plate(coefficient)
    local flexural = modulus * thickness ^ 3 / (12.0 * (1.0 - poisson * poisson))
    return coefficient / (side * side) * math.sqrt(flexural / (density * thickness))
           / (2.0 * nafems.pi)
end
local clamped_square = thin_plate(35.9866)
nafems.note(string.format("a clamped *square* plate of this size and thickness has its first"
                          .. " mode at %.4f Hz", clamped_square))
nafems.note("by thin-plate theory; a 45 degree skew can only raise it.")

local document, body = nafems.open("fv22")
local mesh = nafems.mesh(document, body, {0.6, 0.9, 1.2}, 1, {"sweep", "tetrahedra"})
if not mesh then
    os.exit(nafems.blocked("FV22", "the mesher will not take a skewed planar face"))
end

local study = mep.fem_study{document = document, name = "FV22"}.study
mep.fem_material{study = study, youngs_modulus = modulus, poissons_ratio = poisson,
                 density = density}

-- Every face that is not the top or the bottom: all four sides, clamped.
local held = 0
for _, face in ipairs(nafems.faces(document, body)) do
    if face.high[3] > face.low[3] + 1e-9 then
        mep.fem_support{study = study, face = face.id, label = "clamped edge"}
        held = held + 1
    end
end
nafems.note(string.format("%d side faces clamped", held))

local modal = mep.fem_modal{study = study, mesh = mesh.mesh, modes = 6}
nafems.note(string.format("%d modes, of which %d are rigid-body motions (a clamped plate has none)",
                          #modal.frequencies, modal.rigid_body_modes))
for index = 1, math.min(4, #modal.frequencies) do
    nafems.note(string.format("    mode %d   %9.4f Hz", index, modal.frequencies[index]))
end

nafems.checks = nafems.checks + 1
if modal.frequencies[1] > clamped_square then
    nafems.note(string.format("the first mode is %.1f%% above the clamped square, as a skew"
                              .. " must put it", 100.0 * (modal.frequencies[1] / clamped_square - 1.0)))
else
    nafems.failures = nafems.failures + 1
    nafems.note("the first mode is BELOW the clamped square, which a skew cannot do")
end
nafems.checks = nafems.checks + 1
if modal.rigid_body_modes == 0 then
    nafems.note("no rigid-body modes, as a fully clamped part must have none")
else
    nafems.failures = nafems.failures + 1
    nafems.note("rigid-body modes were reported for a fully clamped part")
end

nafems.still(modal.result, "fv22", {mode = 0, caption = "FV22  FIRST MODE", yaw = 40, pitch = 55})
nafems.film(modal.result, "fv22", {mode = 0, frames = 30, caption = "FV22  CLAMPED RHOMBIC PLATE",
                                   yaw = 40, pitch = 55})

os.exit(nafems.verdict("FV22"))
