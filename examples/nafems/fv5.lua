-- NAFEMS FV5 -- deep simply supported beam, free vibration.
--
-- A 10 m span of 1 x 1 m square section, E = 200 GPa, v = 0.3,
-- rho = 8000 kg/m3, simply supported at both ends.
--
-- A beam is "deep" when its span is few enough multiples of its depth
-- that shear deformation and rotary inertia matter, and FV5 exists
-- because that is where Euler-Bernoulli theory stops being the answer.
-- At ten to one the first mode is already 8% below what Euler-Bernoulli
-- predicts, and the gap widens fast with mode number -- the third mode
-- is out by a third. So the reference is **Timoshenko's**, which is a
-- closed form for simply-supported ends: assuming w = W sin(beta x) and
-- phi = Phi cos(beta x) reduces the coupled pair to a quadratic in
-- omega^2, whose lower root is the bending mode. The upper root is the
-- shear mode, far above.
--
-- Both are printed, because the interesting thing about this benchmark
-- is not that mep agrees with Timoshenko -- it is how far both of them
-- are from the formula anyone would reach for first.

local nafems = dofile((debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or ".") .. "/nafems.lua")

nafems.heading("NAFEMS FV5 -- deep simply supported beam")

local span, depth, width = 10.0, 1.0, 1.0
local modulus, poisson, density = 200e9, 0.3, 8000.0

local document, body = nafems.open("fv5")
local mesh = nafems.mesh(document, body, {0.25, 0.35, 0.5}, 1, {"sweep", "tetrahedra"})
if not mesh then os.exit(nafems.blocked("FV5", "no mesher would take the beam")) end

local study = mep.fem_study{document = document, name = "FV5"}.study
mep.fem_material{study = study, youngs_modulus = modulus, poissons_ratio = poisson,
                 density = density}

-- The two lower end edges, each held in z alone: knife edges, which is
-- what "simply supported" means and what a held *face* would not be.
for _, x in ipairs{0.0, span} do
    local edge, found = nafems.edge_near(document, body, {x, width * 0.5, 0.0}, {0, 1, 0})
    mep.fem_support{study = study, edge = edge, fixed = {false, false, true},
                    label = "knife edge"}
    nafems.note(string.format("knife edge at x = %.1f, %.2f m long", x, found.length))
end

local modal = mep.fem_modal{study = study, mesh = mesh.mesh, modes = 10}
nafems.note(string.format("%d modes, of which %d are rigid-body motions",
                          #modal.frequencies, modal.rigid_body_modes))

-- Euler-Bernoulli, for contrast: the same beam with shear and rotary
-- inertia left out.
local function euler(n)
    local area = depth * width
    local inertia = width * depth ^ 3 / 12.0
    local beta = n * nafems.pi / span
    return math.sqrt(modulus * inertia * beta ^ 4 / (density * area)) / (2.0 * nafems.pi)
end

-- The bending modes are the ones that bend *vertically*; a square
-- section has a lateral family at the same frequencies and the solver
-- returns them interleaved. Picked out by what they are: the vertical
-- ones move in z and the lateral ones in y.
local vertical = {}
local coordinates = mep.fem_nodes{result = modal.result}
for mode = 0, #modal.frequencies - 1 do
    local shape = mep.fem_animate{result = modal.result, mode = mode, frames = 1}.displacement[1]
    local up, sideways = 0.0, 0.0
    for n = 1, coordinates.count do
        up = up + shape[(n - 1) * 3 + 3] ^ 2
        sideways = sideways + shape[(n - 1) * 3 + 2] ^ 2
    end
    if up > 4.0 * sideways and mode >= modal.rigid_body_modes then
        vertical[#vertical + 1] = {mode = mode, frequency = modal.frequencies[mode + 1]}
    end
end

for n = 1, math.min(3, #vertical) do
    local reference = nafems.timoshenko(n, span, depth, width, modulus, poisson, density)
    nafems.compare(string.format("bending mode %d", n), vertical[n].frequency, reference, "Hz",
                   0.05)
    nafems.note(string.format("        Euler-Bernoulli would say %.2f Hz, which is %.1f%% high",
                              euler(n), 100.0 * (euler(n) / reference - 1.0)))
end

if #vertical > 0 then
    nafems.still(modal.result, "fv5", {mode = vertical[1].mode, caption = "FV5  FIRST BENDING",
                                       yaw = 55, pitch = 15})
    nafems.film(modal.result, "fv5", {mode = vertical[1].mode, frames = 30,
                                      caption = "FV5  DEEP SIMPLY SUPPORTED BEAM",
                                      yaw = 55, pitch = 15})
end

os.exit(nafems.verdict("FV5"))
