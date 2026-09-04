#include "html_doc.h"
#include "js_engine.h"
#include "svg_doc.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

DomNode *FindById(DomNode *node, const std::string &id) {
    if (!node) return nullptr;
    if (node->Id() == id) return node;
    for (auto &child : node->children) if (DomNode *found = FindById(child.get(), id)) return found;
    return nullptr;
}
}  // namespace

int main() {
    HtmlDoc doc;
    ParseHtml(R"HTML(
      <style>
        .shell .card { color: red; padding: 1em 2px 3% 4px; }
        main > .card[data-kind~="featured"] { color: blue; width: 50%; box-sizing: border-box; }
        #winner { color: green; margin: 2px auto; border: 3px solid #123456; }
        li:nth-child(2) { font-weight: bold; }
      </style>
      <main class="shell"><div id="winner" class="card" data-kind="new featured">card</div></main>
      <ul><li id="first">one</li><li id="second">two</li></ul>
      <input id="check" type="checkbox" checked><textarea id="memo">hello form</textarea><select id="choice"><option value="a">A</option><option value="b" selected>B</option></select><details id="more" open><summary>More</summary></details>
    )HTML", doc);
    DomNode *winner = FindById(doc.root.get(), "winner");
    DomNode *second = FindById(doc.root.get(), "second");
    DomNode *first = FindById(doc.root.get(), "first");
    DomNode *check = FindById(doc.root.get(), "check");
    DomNode *memo = FindById(doc.root.get(), "memo");
    DomNode *choice = FindById(doc.root.get(), "choice");
    DomNode *more = FindById(doc.root.get(), "more");
    CHECK(winner && second && first && check && memo && choice && more);
    CHECK(winner->style.has_color && winner->style.color_g == 158);
    CHECK(winner->style.width.set && winner->style.width.unit == CssLength::Unit::Percent && winner->style.width.value == 50.0f);
    CHECK(winner->style.border_box);
    CHECK(winner->style.margin.left.auto_value && winner->style.margin.right.auto_value);
    CHECK(winner->style.padding.top.unit == CssLength::Unit::Em && winner->style.padding.top.value == 1.0f);
    CHECK(winner->style.padding.bottom.unit == CssLength::Unit::Percent && winner->style.padding.bottom.value == 3.0f);
    CHECK(winner->style.border_top.present && winner->style.border_top.width_px == 3.0f && winner->style.border_top.r == 0x12);
    CHECK(second->style.bold && !first->style.bold);
    CHECK(check->form_checked && memo->form_value == "hello form" && choice->form_value == "b" && more->details_open);
    doc.scripts = {"document.querySelector('.card').textContent = 'selected'; document.title = document.querySelectorAll('li').length;"};
    RunScripts(doc, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(winner->children.size() == 1 && winner->children[0]->text == "selected");
    CHECK(doc.title == "2");
    doc.scripts = {"var made = document.createElement('p'); made.setAttribute('id', 'made'); made.appendChild(document.createTextNode('created')); document.querySelector('main').appendChild(made);"};
    RunScripts(doc, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    DomNode *made = FindById(doc.root.get(), "made");
    CHECK(made && made->parent && made->parent->tag == "main" && made->children.size() == 1 && made->children[0]->text == "created");
    doc.scripts = {"document.querySelector('#made').classList.add('active'); document.querySelector('#made').style.backgroundColor = 'red'; document.querySelector('#made').value = 'live'; document.title = document.querySelector('#first').nextElementSibling.textContent;"};
    RunScripts(doc, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(made->Class() == "active" && made->form_value == "live" && doc.title == "two" && made->attrs["style"].find("background-color: red") != std::string::npos);
    doc.scripts = {"document.title = window.getComputedStyle(document.querySelector('#winner')).color;"};
    RunScripts(doc, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(doc.title == "#2e9e4a");
    doc.scripts = {"document.querySelector('#made').innerHTML = '<span id=\"nested\">serialized</span>'; document.title = document.querySelector('#made').innerHTML;"};
    RunScripts(doc, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    DomNode *nested = FindById(doc.root.get(), "nested");
    CHECK(nested && nested->parent == made && doc.title.find("serialized") != std::string::npos);
    doc.scripts = {"document.querySelector('#more').open = false;"};
    RunScripts(doc, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(!more->details_open);
    HtmlDoc document_roots;
    ParseHtml("<html><head><title>roots</title></head><body id=\"body\">body</body></html><script>document.title = document.documentElement.tagName + document.head.tagName + document.body.tagName;</script>", document_roots);
    RunScripts(document_roots, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(document_roots.title == "htmlheadbody");
    HtmlDoc shared_scripts;
    ParseHtml("<script>var shared = 'global'; function suffix() { return shared + '-scope'; }</script><script>document.title = suffix();</script>", shared_scripts);
    RunScripts(shared_scripts, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(shared_scripts.title == "global-scope");
    HtmlDoc arrays;
    ParseHtml("<script>var values = [1]; values.push(2); values.unshift(0); values.shift(); values.reverse(); values.reverse(); document.title = values.slice(0, 1).concat(3).join('-') + ':' + values.indexOf(2) + ':' + values.includes(1) + ':' + Array.isArray(values);</script>", arrays);
    RunScripts(arrays, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(arrays.title == "1-3:1:true:true");
    HtmlDoc objects;
    ParseHtml("<script>var target = Object.assign({a: 1}, {b: 2}); document.title = Object.keys(target).length + ':' + Object.values(target).join('-') + ':' + Object.entries(target).length;</script>", objects);
    RunScripts(objects, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(objects.title == "2:1-2:2" || objects.title == "2:2-1:2");
    HtmlDoc math;
    ParseHtml("<script>document.title = Math.abs(-3) + ':' + Math.floor(1.9) + ':' + Math.ceil(1.1) + ':' + Math.max(2, 5);</script>", math);
    RunScripts(math, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(math.title == "3:1:2:5");
    HtmlDoc math_extra;
    ParseHtml("<script>document.title = Math.sqrt(16) + ':' + Math.round(Math.sin(Math.PI / 2)) + ':' + Math.pow(2, 10) + ':' + Math.hypot(3, 4) + ':' + Math.sign(-2) + ':' + (Math.random() < 1);</script>", math_extra);
    RunScripts(math_extra, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(math_extra.title == "4:1:1024:5:-1:true");
    HtmlDoc numbers;
    ParseHtml("<script>document.title = parseInt('42') + ':' + Number.parseFloat('1.5') + ':' + Number.isFinite(3);</script>", numbers);
    RunScripts(numbers, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(numbers.title == "42:1.5:true");
    HtmlDoc canvas;
    ParseHtml("<canvas id=\"paint\" width=\"40\" height=\"20\"></canvas><script>var c = document.getElementById('paint'); var ctx = c.getContext('2d'); ctx.fillStyle = '#123'; ctx.fillRect(1, 2, 3, 4); ctx.strokeStyle = 'red'; ctx.lineWidth = 2; ctx.strokeRect(5, 6, 7, 8); ctx.beginPath(); ctx.moveTo(0, 1); ctx.lineTo(9, 10); ctx.quadraticCurveTo(12, 2, 15, 8); ctx.bezierCurveTo(16, 2, 18, 12, 20, 8); ctx.arc(10, 10, 3, 0, 3.14); ctx.ellipse(10, 8, 3, 2, 0, 0, 6.28); ctx.closePath(); ctx.stroke(); ctx.fill(); ctx.fillText('hi', 2, 12); ctx.save(); ctx.fillStyle = 'blue'; ctx.restore(); var pixels = ctx.createImageData(2, 1); pixels.data[0] = 7; pixels.data[3] = 255; ctx.putImageData(pixels, 4, 5); ctx.beginPath(); ctx.rect(1, 1, 2, 2); document.title = c.width + ':' + c.height + ':' + ctx.fillStyle + ':' + ctx.strokeStyle + ':' + ctx.measureText('ab').width + ':' + pixels.data.length + ':' + pixels.data[3] + ':' + ctx.isPointInPath(1.5, 1.5);</script>", canvas);
    RunScripts(canvas, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    DomNode *paint = FindById(canvas.root.get(), "paint");
    CHECK(paint && paint->canvas_width == 40 && paint->canvas_height == 20 && paint->canvas_commands.size() == 6);
    CHECK(paint->canvas_commands[0].kind == CanvasCommand::Kind::FillRect && paint->canvas_commands[0].r == 0x11 && paint->canvas_commands[0].g == 0x22 && paint->canvas_commands[0].b == 0x33);
    CHECK(paint->canvas_commands[1].kind == CanvasCommand::Kind::StrokeRect && paint->canvas_commands[1].r == 255 && paint->canvas_commands[1].g == 0 && paint->canvas_commands[1].line_width == 2.0f);
    CHECK(paint->canvas_commands[2].kind == CanvasCommand::Kind::StrokePath && paint->canvas_commands[2].points.size() > 120 && paint->canvas_commands[2].r == 255);
    CHECK(paint->canvas_commands[3].kind == CanvasCommand::Kind::FillPath && paint->canvas_commands[3].points.size() > 8 && paint->canvas_commands[3].r == 0x11 && !paint->canvas_commands[3].triangles.empty());
    CHECK(paint->canvas_commands[4].kind == CanvasCommand::Kind::FillText && paint->canvas_commands[4].text == "hi" && paint->canvas_commands[4].font_size == 16.0f);
    CHECK(paint->canvas_commands[5].kind == CanvasCommand::Kind::ImageData && paint->canvas_commands[5].x == 4.0f && paint->canvas_commands[5].pixels[0] == 7 && paint->canvas_commands[5].pixels[3] == 255);
    CHECK(canvas.title == "40:20:#112233:#ff0000:19.2:8:255:true");
    // Transforms are baked in at record time; gradients snapshot their stops;
    // multiple subpaths survive a second moveTo; globalAlpha scales paint.
    HtmlDoc canvas_transform;
    ParseHtml("<canvas id=\"t\" width=\"100\" height=\"100\"></canvas><script>var c = document.getElementById('t'); var ctx = c.getContext('2d'); ctx.translate(10, 20); ctx.scale(2, 2); ctx.fillRect(1, 1, 5, 5); ctx.save(); ctx.rotate(Math.PI / 2); ctx.fillRect(0, 0, 4, 2); ctx.restore(); var g = ctx.createLinearGradient(0, 0, 100, 0); g.addColorStop(1, 'blue'); g.addColorStop(0, 'rgba(255, 0, 0, 0.5)'); ctx.fillStyle = g; ctx.globalAlpha = 0.5; ctx.fillRect(0, 0, 10, 10); ctx.resetTransform(); ctx.beginPath(); ctx.moveTo(0, 0); ctx.lineTo(10, 0); ctx.moveTo(0, 5); ctx.lineTo(10, 5); ctx.strokeStyle = 'rgb(0, 0, 255)'; ctx.stroke(); var m = ctx.getTransform(); document.title = (ctx.fillStyle === g) + ':' + m.a + ':' + m.e + ':' + ctx.globalAlpha;</script>", canvas_transform);
    RunScripts(canvas_transform, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    DomNode *transformed = FindById(canvas_transform.root.get(), "t");
    CHECK(transformed && transformed->canvas_commands.size() == 5 && canvas_transform.title == "true:1:0:0.5");
    const CanvasCommand &scaled = transformed->canvas_commands[0];
    CHECK(scaled.kind == CanvasCommand::Kind::FillRect && scaled.x == 12.0f && scaled.y == 22.0f && scaled.w == 10.0f && scaled.h == 10.0f);
    const CanvasCommand &rotated_rect = transformed->canvas_commands[1];
    CHECK(rotated_rect.kind == CanvasCommand::Kind::FillPath && rotated_rect.points.size() == 8 && rotated_rect.triangles.size() == 6);
    CHECK(std::fabs(rotated_rect.points[2] - 10.0f) < 0.01f && std::fabs(rotated_rect.points[3] - 28.0f) < 0.01f);  // (4,0) rotated 90deg then scaled/translated
    const CanvasCommand &shaded = transformed->canvas_commands[2];
    CHECK(shaded.gradient.present && !shaded.gradient.radial && shaded.gradient.stops.size() == 2 && shaded.gradient.stops[0].offset == 0.0f && shaded.gradient.stops[0].r == 255 && shaded.gradient.stops[0].a == 63 && shaded.gradient.stops[1].b == 255 && shaded.gradient.stops[1].a == 127);
    unsigned char gr, gg, gb, ga;
    CHECK(CanvasGradientColorAt(shaded.gradient, 50.0f, 0.0f, gr, gg, gb, ga) && gr == 127 && gb == 127 && ga == 95);
    CHECK(CanvasGradientColorAt(shaded.gradient, -5.0f, 0.0f, gr, gg, gb, ga) && gr == 255 && gb == 0);
    CHECK(transformed->canvas_commands[3].kind == CanvasCommand::Kind::StrokePath && transformed->canvas_commands[4].kind == CanvasCommand::Kind::StrokePath && transformed->canvas_commands[4].points[1] == 5.0f && transformed->canvas_commands[4].b == 255 && transformed->canvas_commands[4].a == 127);
    HtmlDoc accessibility;
    ParseHtml("<main aria-label=\"Workspace\"><h1 id=\"heading\">Welcome</h1><span id=\"help\">Saves your work</span><button disabled>Save</button><input type=\"checkbox\" checked aria-label=\"Publish\"><button aria-labelledby=\"heading\" aria-describedby=\"help\">ignored</button></main>", accessibility);
    AccessibleNode accessible_root = BuildAccessibilityTree(accessibility);
    CHECK(accessible_root.role == "document" && accessible_root.children.size() == 1);
    CHECK(accessible_root.children[0].role == "main" && accessible_root.children[0].name == "Workspace");
    CHECK(accessible_root.children[0].children.size() == 5 && accessible_root.children[0].children[2].role == "button" && accessible_root.children[0].children[2].disabled);
    CHECK(accessible_root.children[0].children[3].role == "checkbox" && accessible_root.children[0].children[3].name == "Publish" && accessible_root.children[0].children[3].checked);
    CHECK(accessible_root.children[0].children[4].name == "Welcome" && accessible_root.children[0].children[4].description == "Saves your work");
    HtmlDoc hidden_accessibility;
    ParseHtml("<main><button aria-hidden=\"true\">Hidden</button><button>Visible</button></main>", hidden_accessibility);
    AccessibleNode hidden_root = BuildAccessibilityTree(hidden_accessibility);
    CHECK(hidden_root.children.size() == 1 && hidden_root.children[0].children.size() == 1 && hidden_root.children[0].children[0].name == "Visible");
    HtmlDoc shadow;
    ParseHtml("<div id=\"host\"><b slot=\"title\">light</b></div><script>var host = document.getElementById('host'); var root = host.attachShadow({mode: 'open'}); root.innerHTML = '<slot name=\"title\">fallback</slot>'; document.title = host.shadowRoot.children[0].tagName;</script>", shadow);
    RunScripts(shadow, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    DomNode *host = FindById(shadow.root.get(), "host");
    CHECK(host && host->shadow_root && host->shadow_root->children.size() == 1 && host->shadow_root->children[0]->tag == "slot" && shadow.title == "slot");
    HtmlDoc custom_elements;
    ParseHtml("<script>var definition = {}; customElements.define('x-card', definition); document.title = customElements.get('x-card') == definition;</script>", custom_elements);
    RunScripts(custom_elements, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(custom_elements.title == "true");
    HtmlDoc wasm;
    // Empty module; garbage; a real (module (func)) with type/function/code
    // sections; the same with a leading custom section; sections out of
    // order; function/code count mismatch; a section size past the end.
    ParseHtml("<script>var head = [0, 97, 115, 109, 1, 0, 0, 0]; var body = [1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 4, 1, 2, 0, 11];"
              "document.title = WebAssembly.validate(head) + ':' + WebAssembly.validate([1, 2]) + ':' + WebAssembly.validate(head.concat(body))"
              " + ':' + WebAssembly.validate(head.concat([0, 3, 2, 104, 105]).concat(body)) + ':' + WebAssembly.validate(head.concat([10, 4, 1, 2, 0, 11, 3, 2, 1, 0]))"
              " + ':' + WebAssembly.validate(head.concat([3, 2, 1, 0])) + ':' + WebAssembly.validate(head.concat([1, 9, 1]));</script>", wasm);
    RunScripts(wasm, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(wasm.title == "true:false:true:true:false:false:false");
    HtmlDoc svg_dom;
    ParseHtml("<html><body></body></html><script>var rect = document.createElementNS('http://www.w3.org/2000/svg', 'rect'); rect.setAttribute('width', 12); document.body.appendChild(rect); document.title = rect.tagName + ':' + rect.getAttribute('xmlns') + ':' + rect.getAttribute('width');</script>", svg_dom);
    RunScripts(svg_dom, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(svg_dom.title == "rect:http://www.w3.org/2000/svg:12");
    HtmlDoc media;
    ParseHtml("<audio id=\"track\"></audio><script>var track = document.getElementById('track'); track.play(); track.currentTime = 4.5; track.volume = 2; track.muted = true; document.title = track.paused + ':' + track.currentTime + ':' + track.volume + ':' + track.muted; track.pause();</script>", media);
    RunScripts(media, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    DomNode *track = FindById(media.root.get(), "track");
    CHECK(track && track->media_paused && track->media_current_time == 4.5 && track->media_volume == 1.0 && track->media_muted && media.title == "false:4.5:1:true");
    // --- media pipeline: a real PCM16 WAV on disk drives duration/readyState/ended ---
    {
        std::filesystem::path wav_path = std::filesystem::temp_directory_path() / "mep_html_doc_test.wav";
        const int rate = 8000, frames = 4000;  // 0.5 s mono
        std::vector<unsigned char> wav;
        auto put32 = [&wav](unsigned v) { for (int i = 0; i < 4; ++i) wav.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xffU)); };
        auto put16 = [&wav](unsigned v) { for (int i = 0; i < 2; ++i) wav.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xffU)); };
        wav.insert(wav.end(), {'R', 'I', 'F', 'F'}); put32(36 + frames * 2); wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
        wav.insert(wav.end(), {'f', 'm', 't', ' '}); put32(16); put16(1); put16(1); put32(rate); put32(rate * 2); put16(2); put16(16);
        wav.insert(wav.end(), {'d', 'a', 't', 'a'}); put32(frames * 2);
        for (int i = 0; i < frames; ++i) put16(static_cast<unsigned>(i % 2 ? 1000 : 64536));
        { std::ofstream out(wav_path, std::ios::binary); out.write(reinterpret_cast<const char *>(wav.data()), static_cast<std::streamsize>(wav.size())); }
        HtmlDoc page;
        ParseHtml("<audio id=\"clip\" src=\"mep_html_doc_test.wav\"></audio><video id=\"movie\" src=\"mep_html_doc_test.wav\"></video><audio id=\"missing\"><source src=\"nope.wav\"></audio>"
                  "<script>var clip = document.getElementById('clip'); clip.play(); document.title = clip.duration + ':' + clip.readyState + ':' + clip.canPlayType('audio/wav') + ':' + clip.canPlayType('audio/mpeg') + ':' + (clip.error === null) + ':' + (document.getElementById('movie').error !== null);</script>", page);
        LoadHtmlMedia(page, wav_path.parent_path().string());
        RunScripts(page, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
        DomNode *clip = FindById(page.root.get(), "clip");
        DomNode *missing = FindById(page.root.get(), "missing");
        CHECK(clip && clip->media_ready_state == 4 && clip->media_duration == 0.5 && clip->media_source_path == wav_path.string() && !clip->media_paused);
        CHECK(missing && missing->media_ready_state == 0 && missing->media_error.find("cannot open") != std::string::npos);
        CHECK(page.title == "0.5:4:probably::true:true");
        AdvanceHtmlMediaClock(page, 0.3);
        CHECK(!clip->media_paused && !clip->media_ended && std::fabs(clip->media_current_time - 0.3) < 1e-9);
        AdvanceHtmlMediaClock(page, 0.3);
        CHECK(clip->media_paused && clip->media_ended && clip->media_current_time == 0.5);
        page.scripts = {"var clip = document.getElementById('clip'); clip.loop = true; clip.play(); document.title = clip.currentTime + ':' + clip.ended + ':' + clip.loop;"};
        RunScripts(page, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
        CHECK(page.title == "0:false:true");
        AdvanceHtmlMediaClock(page, 0.6);
        CHECK(!clip->media_paused && !clip->media_ended && std::fabs(clip->media_current_time - 0.1) < 1e-6);
        std::filesystem::remove(wav_path);
    }
    // --- SVG display list (svg_doc.cpp) ---
    {
        std::vector<SvgSubpath> subpaths = ParseSvgPathData("M1 2 L3 4 5 6 Z M7 8");
        CHECK(subpaths.size() == 2 && subpaths[0].closed && subpaths[0].points.size() == 6 && subpaths[0].points[4] == 5.0f && !subpaths[1].closed && subpaths[1].points.size() == 2);
        std::vector<SvgSubpath> glued = ParseSvgPathData("M0 0A1 1 0 1110 0");  // arc flags run together with the endpoint
        CHECK(glued.size() == 1 && glued[0].points.size() > 4 && glued[0].points[glued[0].points.size() - 2] == 10.0f && glued[0].points.back() == 0.0f);
        std::vector<SvgSubpath> curves = ParseSvgPathData("m0,0 c1,1 2,1 3,0 s2,-1 3,0 q1,1 2,0 t2,0");
        CHECK(curves.size() == 1 && curves[0].points.size() == 2 * (1 + 16 + 16 + 12 + 12) && curves[0].points[curves[0].points.size() - 2] == 10.0f);
        std::vector<unsigned> tris = TriangulateSvgPolygon({0, 0, 4, 0, 4, 1, 1, 1, 1, 4, 0, 4});  // concave "L"
        CHECK(tris.size() == 12);
        const float pts[] = {0, 0, 4, 0, 4, 1, 1, 1, 1, 4, 0, 4};
        for (size_t i = 0; i < tris.size(); i += 3) {
            float ax = pts[tris[i] * 2], ay = pts[tris[i] * 2 + 1], bx = pts[tris[i + 1] * 2], by = pts[tris[i + 1] * 2 + 1], cx = pts[tris[i + 2] * 2], cy = pts[tris[i + 2] * 2 + 1];
            CHECK((bx - ax) * (cy - ay) - (by - ay) * (cx - ax) < 0.0f);  // screen counter-clockwise, raylib's DrawTriangle order
            CHECK(!((ax + bx + cx) / 3.0f > 1.0f && (ay + by + cy) / 3.0f > 1.0f));  // no triangle covers the notch
        }
    }
    HtmlDoc svg;
    ParseHtml(R"HTML(<svg id="icon" viewBox="0 0 24 24" width="48"><g fill="red" transform="translate(2 2)"><rect x="0" y="0" width="10" height="10"/><circle cx="5" cy="5" r="2" fill="none" stroke="#00f" stroke-width="2"/></g><path d="M0 20 h4 v-4 z" fill="currentColor"/><path d="M10 10 C 12 0, 18 0, 20 10 A5 5 0 1 0 15 15 q1 1 2 2 t 2 2" fill="none" style="stroke: rgb(0, 128, 0); opacity: 0.5"/><text x="12" y="20" text-anchor="middle" font-size="4">Hi</text><defs><linearGradient id="g"><stop stop-color="#fff"/><stop stop-color="#000"/></linearGradient></defs><rect x="0" y="0" width="1" height="1" fill="url(#g)"/><use href="#missing"/><rect display="none" width="5" height="5"/></svg>)HTML", svg);
    DomNode *icon = FindById(svg.root.get(), "icon");
    float intrinsic_w = 0, intrinsic_h = 0;
    CHECK(icon && SvgIntrinsicSize(*icon, intrinsic_w, intrinsic_h) && intrinsic_w == 48.0f && intrinsic_h == 48.0f);
    SvgDisplayList list = BuildSvgDisplayList(*icon, 48.0f, 48.0f, SvgPaint{true, 9, 8, 7, 255});
    CHECK(list.shapes.size() == 6);
    CHECK(list.shapes[0].kind == SvgShape::Kind::Polygon && list.shapes[0].fill.r == 255 && list.shapes[0].fill.g == 0 && list.shapes[0].points.size() == 8 && list.shapes[0].points[0] == 4.0f && list.shapes[0].points[4] == 24.0f && list.shapes[0].triangles.size() == 6 && !list.shapes[0].stroke.present);
    CHECK(list.shapes[1].kind == SvgShape::Kind::Polyline && list.shapes[1].closed && !list.shapes[1].fill.present && list.shapes[1].stroke.b == 255 && list.shapes[1].stroke_width == 4.0f && list.shapes[1].points.size() >= 24);
    CHECK(list.shapes[2].kind == SvgShape::Kind::Polygon && list.shapes[2].fill.r == 9 && list.shapes[2].fill.b == 7 && list.shapes[2].points.size() == 6 && list.shapes[2].points[1] == 40.0f && list.shapes[2].triangles.size() == 3);
    CHECK(list.shapes[3].kind == SvgShape::Kind::Polyline && !list.shapes[3].closed && list.shapes[3].stroke.g == 128 && list.shapes[3].stroke.a == 127 && list.shapes[3].points.size() > 80);
    CHECK(list.shapes[4].kind == SvgShape::Kind::Text && list.shapes[4].text == "Hi" && list.shapes[4].points[0] == 24.0f && list.shapes[4].points[1] == 40.0f && list.shapes[4].font_size == 8.0f && list.shapes[4].text_anchor == "middle");
    CHECK(list.shapes[5].fill.r == 127 && list.shapes[5].fill.g == 127 && list.shapes[5].points.size() == 8);
    HtmlDoc svg_rotated;
    ParseHtml("<svg id=\"r\" width=\"10\" height=\"10\"><rect width=\"10\" height=\"10\" transform=\"rotate(90 5 5)\" fill=\"#123456\"/><symbol id=\"s\"><circle r=\"1\"/></symbol><use href=\"#s\" x=\"3\" y=\"3\"/></svg>", svg_rotated);
    SvgDisplayList rotated = BuildSvgDisplayList(*FindById(svg_rotated.root.get(), "r"), 20.0f, 20.0f);
    CHECK(rotated.shapes.size() == 2 && rotated.shapes[0].fill.r == 0x12 && rotated.shapes[0].fill.b == 0x56);
    // The rotated square still covers the same 20x20 box after scaling by 2.
    CHECK(std::fabs(rotated.shapes[0].points[0] - 20.0f) < 0.01f && std::fabs(rotated.shapes[0].points[1]) < 0.01f);
    CHECK(rotated.shapes[1].kind == SvgShape::Kind::Polygon && std::fabs(rotated.shapes[1].points[0] - 8.0f) < 0.01f && std::fabs(rotated.shapes[1].points[1] - 6.0f) < 0.01f);
    std::cout << "html_doc_test passed\n";
}
