-- A viewer of a cantilever's mode shapes, animated by time.
--
--     :Viewer examples/viewer_modes.lua
--     :vsplit examples/viewer_modes.lua      -- and edit it; :w rebuilds
--
-- Drag to turn, shift-drag or middle-drag to pan, wheel to zoom, drag
-- the slider to move through the cycle, `p` to play. `f` frames the
-- scene, `w` toggles the mesh, `l` the legend.
--
-- WHAT THE TIME IS HERE, AND WHY IT IS NOT SECONDS. A mode shape is an
-- eigenvector: it has a direction and no magnitude. The amplitude drawn
-- below is chosen from the geometry -- a fraction of the part's own
-- diagonal -- because the eigenvector's own magnitude is a number in
-- units of one over the square root of mass, with no interpretation as a
-- displacement at all. So the slider runs over a *phase*, 0 to 1 of one
-- cycle, and the frequency is printed rather than being the clock. That
-- is the same reasoning fem_animate.h sets out at length; it is the one
-- thing about animating a mode that is easy to get quietly wrong.

local WHICH = 0        -- which mode, from zero. Change it and press :w
local LENGTH = 4.0
local SECTION = 0.4

local document = mep.part_new{title = "cantilever"}.document
local body = mep.part_box{document = document, size = {LENGTH, SECTION, SECTION}}.body

-- Swept into hexahedra: a beam in bending is exactly the case four-node
-- tetrahedra are worst at, and a box is exactly the case the sweeper is
-- best at.
local mesh = mep.fem_mesh{document = document, body = body, size = 0.12, method = "sweep"}
if not mesh then error("the beam would not mesh") end

local study = mep.fem_study{document = document, name = "cantilever"}.study
mep.fem_material{study = study, youngs_modulus = 210e9, poissons_ratio = 0.3, density = 7850}
local root = mep.part_face_at{document = document, body = body, normal = {-1, 0, 0}}
mep.fem_support{study = study, face = root.face, label = "clamped"}

local modal = mep.fem_modal{study = study, mesh = mesh.mesh, modes = 6}
if not modal then error("the modal solve failed") end

local frequency = modal.frequencies[WHICH + 1] or 0
view.caption(string.format("CANTILEVER  MODE %d  %.4g HZ", WHICH + 1, frequency))
view.time_range(0, 1)

view.on_frame(function(t)
    -- EVERYTHING GOES IN HERE, not some of it outside. `on_frame` gets a
    -- cleared scene each time it is called, which is what lets it be
    -- written as "what is here now" rather than as a diff -- and which
    -- means anything added outside it is wiped the first time the time
    -- moves. That is the one rule of writing one of these.
    --
    -- One full cycle over the slider, so playing it loops seamlessly.
    view.mode{result = modal.result, mode = WHICH, phase = 2 * math.pi * t, amplitude = 0.10}

    -- The root, marked, because where a mode is *held* is what makes it
    -- that mode rather than a rigid motion.
    view.line({0, -SECTION, -SECTION}, {0, SECTION * 2, -SECTION}, view.colors.orange)
    view.line({0, -SECTION, SECTION * 2}, {0, SECTION * 2, SECTION * 2}, view.colors.orange)
    view.label({0, 0, SECTION * 2.4}, "CLAMPED", view.colors.orange)

    -- Every frequency, as labels standing off the tip, so the numbers
    -- are in the picture rather than in a message that scrolls away.
    -- The one being drawn is picked out.
    for i, hz in ipairs(modal.frequencies) do
        local colour = (i == WHICH + 1) and view.colors.yellow or view.colors.grey
        -- Stacked above the clamped end. A label is anchored in the
        -- *world*, not to the frame, so where it lands depends on the
        -- camera -- and one placed off to the side walks out of the
        -- picture the moment the pane is narrow or the view turns.
        -- Above the part, within the part's own extent, is the position
        -- that survives both.
        view.label({LENGTH * 0.05, 0, SECTION * (7.0 - 0.9 * i)},
                   string.format("%d  %.4g HZ", i, hz), colour)
    end
end)

view.camera{yaw = 55, pitch = 18, distance = LENGTH * 1.4}
