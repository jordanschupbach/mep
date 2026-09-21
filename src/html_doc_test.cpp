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
        #winner { color: green; margin: 2px auto; border: 3px solid #123456; font-family: serif; text-align: center; line-height: 2; letter-spacing: 2px; white-space: nowrap; }
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
    CHECK(winner->style.font_family == HtmlFontFamily::Serif);
    CHECK(winner->style.text_align == HtmlTextAlign::Center);
    CHECK(winner->style.line_height_multiplier == 2.0f && winner->style.letter_spacing.set &&
          winner->style.letter_spacing.value == 2.0f && winner->style.white_space == HtmlWhiteSpace::NoWrap);
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
    CHECK(document_roots.title == "HTMLHEADBODY");  // tagName is uppercase for HTML elements
    HtmlDoc modern_operators;
    ParseHtml("<script>var missing = null?.name ?? 'fallback'; var zero = ({x: 0})?.x ?? 7; var called = null?.fn() ?? 'not-called'; document.title = missing + ':' + zero + ':' + called;</script>", modern_operators);
    RunScripts(modern_operators, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(modern_operators.title == "fallback:0:not-called");
    HtmlDoc iteration;
    ParseHtml("<script>var sum = 0; for (let value of [1, 2, 3]) { sum += value; } var count = 0; for (var key in {a: 1, b: 2}) { count++; } var last = 0; for (last of [4]) {} document.title = sum + ':' + count + ':' + last;</script>", iteration);
    RunScripts(iteration, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(iteration.title == "6:2:4");
    HtmlDoc switch_doc;
    ParseHtml("<script>var result = ''; switch (2) { case 1: result = 'one'; break; case 2: result = 'two'; case 3: result += '-fallthrough'; break; default: result = 'default'; } document.title = result;</script>", switch_doc);
    RunScripts(switch_doc, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(switch_doc.title == "two-fallthrough");
    HtmlDoc labels;
    ParseHtml("<script>var sum = 0; outer: for (let i = 0; i < 3; i++) { for (let j = 0; j < 3; j++) { if (j == 1) continue outer; sum++; } } done: { sum++; break done; sum += 100; } document.title = sum;</script>", labels);
    RunScripts(labels, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(labels.title == "4");
    HtmlDoc default_params;
    ParseHtml("<script>function greet(name = 'world', punctuation = '!') { return name + punctuation; } var add = function(a, b = a) { return a + b; }; var twice = (n = 2) => n * 2; var x = 4; var packed = {x}; var property = 'answer'; var enhanced = {[property]: 42, triple(n) { return n * 3; }}; document.title = greet() + ':' + greet('mep', '?') + ':' + add(3) + ':' + twice() + ':' + packed.x + ':' + enhanced.answer + ':' + enhanced.triple(2);</script>", default_params);
    RunScripts(default_params, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(default_params.title == "world!:mep?:6:4:4:42:6");
    HtmlDoc spread_rest;
    ParseHtml("<script>function sum(first, ...rest) { return first + rest[0] + rest[1]; } var tailCount = (...items) => items.length; var tail = [2, 3]; var values = [1, ...tail, 4]; var base = {a: 1}; var extended = {...base, b: 2}; document.title = values.length + ':' + sum(...values) + ':' + tailCount(...values) + ':' + extended.a + ':' + extended.b;</script>", spread_rest);
    RunScripts(spread_rest, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(spread_rest.title == "4:6:4:1:2");
    HtmlDoc destructuring;
    ParseHtml("<script>var [first, , {value: nested = 9}] = [1, 2, {}]; var {a, b: renamed = 4, deep: [last]} = {a: 3, deep: [7]}; document.title = first + ':' + nested + ':' + a + ':' + renamed + ':' + last;</script>", destructuring);
    RunScripts(destructuring, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(destructuring.title == "1:9:3:4:7");
    HtmlDoc destructuring_assignment;
    ParseHtml("<script>var x = 0, y = 0, label = ''; ([x, {deep: [y = 8]}] = [5, {deep: []}]); ({label} = {label: 'named'}); document.title = x + ':' + y + ':' + label;</script>", destructuring_assignment);
    RunScripts(destructuring_assignment, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(destructuring_assignment.title == "5:8:named");
    HtmlDoc destructuring_params;
    ParseHtml("<script>function describe([first, {name = 'none'}], {count}) { return first + ':' + name + ':' + count; } var read = ({value}) => value; document.title = describe([2, {}], {count: 3}) + ':' + read({value: 4});</script>", destructuring_params);
    RunScripts(destructuring_params, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(destructuring_params.title == "2:none:3:4");
    HtmlDoc classes;
    ParseHtml("<script>class Counter { static count = 2; constructor(value) { this.value = value; } add(amount) { this.value += amount; return this.value; } static named() { return 'counter'; } } var counter = new Counter(2); document.title = counter.add(3) + ':' + Counter.named() + ':' + Counter.count;</script>", classes);
    RunScripts(classes, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(classes.title == "5:counter:2");
    HtmlDoc class_inheritance;
    ParseHtml("<script>class Base { constructor(value) { this.value = value; } describe() { return this.value; } } class Child extends Base { constructor(value) { super(value + 1); } describe() { return super.describe() + 1; } double() { return this.value * 2; } } var child = new Child(4); document.title = child.describe() + ':' + child.double();</script>", class_inheritance);
    RunScripts(class_inheritance, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(class_inheritance.title == "6:10");
    HtmlDoc class_accessors;
    ParseHtml("<script>class Box { constructor(value) { this.raw = value; } get value() { return this.raw + 1; } set value(next) { this.raw = next * 2; } } var box = new Box(2); var before = box.value; box.value = 4; document.title = before + ':' + box.value;</script>", class_accessors);
    RunScripts(class_accessors, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(class_accessors.title == "3:9");
    HtmlDoc class_private_fields;
    ParseHtml("<script>class Secret { #value = 2; constructor(value) { this.#value = value; } reveal() { return this.#value; } } class DefaultSecret { #value = 3; reveal() { return this.#value; } } var secret = new Secret(7); var defaultSecret = new DefaultSecret(); document.title = secret.reveal() + ':' + defaultSecret.reveal();</script>", class_private_fields);
    RunScripts(class_private_fields, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(class_private_fields.title == "7:3");
    HtmlDoc generators;
    ParseHtml("<script>function* sequence(start) { yield start; yield start + 1; } var iterator = sequence(4); var first = iterator.next(); var second = iterator.next(); var done = iterator.next(); document.title = first.value + ':' + first.done + ':' + second.value + ':' + done.done;</script>", generators);
    RunScripts(generators, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(generators.title == "4:false:5:true");
    HtmlDoc object_prototypes;
    ParseHtml("<script>var proto = {greet: function() { return this.name; }}; var item = Object.create(proto); item.name = 'mep'; var same = Object.getPrototypeOf(item) === proto; Object.setPrototypeOf(item, proto); var data = {value: 2}; Object.defineProperty(data, 'double', {get: function() { return this.value * 2; }, set: function(next) { this.value = next / 2; }}); var before = data.double; data.double = 10; var descriptor = Object.getOwnPropertyDescriptor(data, 'double'); Object.freeze(item); item.name = 'blocked'; document.title = item.greet() + ':' + same + ':' + before + ':' + data.value + ':' + (descriptor.get !== undefined) + ':' + item.hasOwnProperty('name') + ':' + item.toString() + ':' + (Object.getPrototypeOf({}) === Object.prototype);</script>", object_prototypes);
    RunScripts(object_prototypes, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(object_prototypes.title == "mep:true:4:5:true:true:[object Object]:true");
    HtmlDoc array_mutators;
    ParseHtml("<script>var values = [3, 1, 2]; var removed = values.splice(1, 1, 8, 9); values.sort(); var flattened = [1, [2, [3]]].flat(2); document.title = removed[0] + ':' + values.join(',') + ':' + flattened.join(',');</script>", array_mutators);
    RunScripts(array_mutators, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(array_mutators.title == "1:2,3,8,9:1,2,3");
    HtmlDoc array_callbacks;
    ParseHtml("<script>var values = [1, 2, 3, 4]; var mapped = values.map(function(value) { return value * 2; }); var filtered = mapped.filter(function(value) { return value > 4; }); var total = filtered.reduce(function(sum, value) { return sum + value; }, 0); var found = values.find(function(value) { return value > 2; }); var index = values.findIndex(function(value) { return value === 4; }); document.title = filtered.join(',') + ':' + total + ':' + found + ':' + index + ':' + values.some(function(value) { return value === 2; }) + ':' + values.every(function(value) { return value > 0; });</script>", array_callbacks);
    RunScripts(array_callbacks, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(array_callbacks.title == "6,8:14:3:3:true:true");
    HtmlDoc string_methods;
    ParseHtml("<script>var text = '  hello world  '; var words = text.trim().split(' '); document.title = words[0].toUpperCase() + ':' + words[1].slice(1) + ':' + 'x'.padStart(3, '0') + ':' + 'ha'.repeat(2) + ':' + 'one one'.replaceAll('one', 'two') + ':' + 'hello'.includes('ell');</script>", string_methods);
    RunScripts(string_methods, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(string_methods.title == "HELLO:orld:00x:haha:two two:true");
    HtmlDoc tagged_templates;
    ParseHtml("<script>function tag(parts, name) { return '[' + parts[0] + name + parts[1] + ']'; } var name = 'mep'; document.title = tag`hello ${name}!`;</script>", tagged_templates);
    RunScripts(tagged_templates, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(tagged_templates.title == "[hello mep!]");
    HtmlDoc number_predicates;
    ParseHtml("<script>document.title = Number.isInteger(3) + ':' + Number.isInteger(3.5) + ':' + Number.isFinite(4) + ':' + (3.14159).toFixed(2) + ':' + (12.345).toPrecision(4);</script>", number_predicates);
    RunScripts(number_predicates, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(number_predicates.title == "true:false:true:3.14:12.35");
    HtmlDoc json_values;
    ParseHtml("<script>var parsed = JSON.parse('{\"name\":\"mep\",\"values\":[1,true,null]}'); document.title = parsed.name + ':' + parsed.values[0] + ':' + JSON.stringify({ok: true, items: [2, 'x']});</script>", json_values);
    RunScripts(json_values, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(json_values.title == "mep:1:{\"ok\":true,\"items\":[2,\"x\"]}" || json_values.title == "mep:1:{\"items\":[2,\"x\"],\"ok\":true}");
    HtmlDoc regexp;
    ParseHtml("<script>var expression = RegExp('(me)p', 'i'); var match = expression.exec('xxMEPyy'); var global = RegExp('a', 'g'); var first = global.exec('aba'); var second = global.exec('aba'); document.title = expression.test('Mep') + ':' + match[0] + ':' + match[1] + ':' + match.index + ':' + 'mep mep'.replaceAll(expression, 'X') + ':' + 'mep'.match(expression)[1] + ':' + first.index + ':' + second.index + ':' + global.lastIndex;</script>", regexp);
    RunScripts(regexp, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(regexp.title == "true:MEP:ME:2:X X:me:0:2:3");
    HtmlDoc function_helpers;
    ParseHtml("<script>function add(a, b) { return this.base + a + b; } var context = {base: 2}; var bound = add.bind(context, 3); document.title = add.call(context, 1, 2) + ':' + add.apply(context, [2, 3]) + ':' + bound(4);</script>", function_helpers);
    RunScripts(function_helpers, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(function_helpers.title == "5:7:9");
    HtmlDoc collections;
    ParseHtml("<script>var map = new Map([['a', 1]]); map.set('b', 2); var set = new Set([1, 2, 1]); set.add(3); var weak = new WeakMap(); weak.set('key', 4); var total = 0; for (var item of set) total += item; var key = ''; for (var pair of map) key += pair[0]; document.title = map.get('a') + ':' + map.size + ':' + set.has(2) + ':' + set.size + ':' + map.delete('b') + ':' + weak.get('key') + ':' + total + ':' + key;</script>", collections);
    RunScripts(collections, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(collections.title == "1:2:true:3:true:4:6:ab");
    HtmlDoc error_objects;
    ParseHtml("<script>var error = new TypeError('invalid value'); document.title = error.name + ':' + error.message + ':' + error.stack;</script>", error_objects);
    RunScripts(error_objects, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(error_objects.title == "TypeError:invalid value:TypeError: invalid value");
    HtmlDoc exceptions;
    ParseHtml("<script>var order = ''; try { throw new RangeError('bad'); } catch (error) { order = error.name + ':' + error.message; } finally { order += ':finally'; } document.title = order;</script>", exceptions);
    RunScripts(exceptions, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(exceptions.title == "RangeError:bad:finally");
    HtmlDoc reflect;
    ParseHtml("<script>var value = {a: 1}; Reflect.set(value, 'b', 2); var keys = Reflect.ownKeys(value); document.title = Reflect.get(value, 'b') + ':' + Reflect.has(value, 'a') + ':' + Reflect.deleteProperty(value, 'a') + ':' + Reflect.has(value, 'a') + ':' + keys.length;</script>", reflect);
    RunScripts(reflect, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(reflect.title == "2:true:true:false:2");
    HtmlDoc symbols;
    ParseHtml("<script>var first = Symbol('token'); var second = Symbol('token'); document.title = (first === second) + ':' + (Symbol.iterator === Symbol.iterator) + ':' + first.description;</script>", symbols);
    RunScripts(symbols, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(symbols.title == "false:true:token");
    HtmlDoc proxy;
    ParseHtml("<script>var target = {value: 2}; var proxy = new Proxy(target, {get: function(object, key) { return key === 'value' ? object[key] * 2 : object[key]; }, set: function(object, key, value) { object[key] = value + 1; return true; }}); var before = proxy.value; proxy.value = 4; document.title = before + ':' + target.value;</script>", proxy);
    RunScripts(proxy, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(proxy.title == "4:5");
    HtmlDoc storage;
    ParseHtml("<script>localStorage.setItem('theme', 'dark'); sessionStorage.setItem('step', 2); var theme = window.localStorage.getItem('theme'); sessionStorage.removeItem('step'); document.title = theme + ':' + (sessionStorage.getItem('step') === null);</script>", storage);
    RunScripts(storage, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(storage.title == "dark:true");
    HtmlDoc cookies;
    ParseHtml("<script>document.cookie = 'theme=dark'; document.title = document.cookie;</script>", cookies);
    RunScripts(cookies, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(cookies.title == "theme=dark");
    HtmlDoc history;
    ParseHtml("<script>history.pushState({page: 2}, '', '/settings?tab=general#theme'); window.location = '/home#top'; document.title = window.location.pathname + ':' + location.search + ':' + location.hash + ':' + history.state.page;</script>", history);
    RunScripts(history, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(history.title == "/home::#top:2");
    HtmlDoc history_navigation;
    ParseHtml("<script>var event_log = ''; window.onpopstate = function(event) { event_log = event.state.page + ':' + location.pathname; }; history.pushState({page: 1}, '', '/one'); history.pushState({page: 2}, '', '/two'); history.back(); document.title = event_log + ':' + history.state.page + ':' + history.length;</script>", history_navigation);
    RunScripts(history_navigation, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(history_navigation.title == "1:/one:1:3");
    HtmlDoc location_methods;
    ParseHtml("<script>location.assign('/first?query=yes#anchor'); location.replace('/final#done'); location.reload(); document.title = location.href + ':' + location.pathname + ':' + location.search + ':' + location.hash;</script>", location_methods);
    RunScripts(location_methods, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(location_methods.title == "/final#done:/final::#done");
    HtmlDoc focus;
    ParseHtml("<style>input:focus { color: red; }</style><input id=\"field\"><script>document.getElementById('field').focus(); document.title = document.activeElement.tagName;</script>", focus);
    RunScripts(focus, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    DomNode *focused_field = FindById(focus.root.get(), "field");
    CHECK(focused_field && focused_field->interaction_focus && focus.title == "INPUT" && focused_field->style.has_color && focused_field->style.color_r == 0xdc && focused_field->style.color_g == 0x32);
    HtmlDoc microtasks;
    ParseHtml("<script>var order = 'sync'; queueMicrotask(function() { order += ':first'; queueMicrotask(function() { document.title = order + ':second'; }); });</script>", microtasks);
    RunScripts(microtasks, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(microtasks.title == "sync:first:second");
    HtmlDoc timers;
    ParseHtml("<script>var order = 'sync'; queueMicrotask(function() { order += ':micro'; }); var canceled = setTimeout(function() { order += ':canceled'; }, 0); clearTimeout(canceled); setTimeout(function(label) { document.title = order + ':' + label; }, 0, 'timer');</script>", timers);
    RunScripts(timers, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(timers.title == "sync:micro:timer");
    HtmlDoc animation_frames;
    ParseHtml("<script>var canceled = requestAnimationFrame(function() { document.title = 'canceled'; }); cancelAnimationFrame(canceled); requestAnimationFrame(function(timestamp) { document.title = typeof timestamp; });</script>", animation_frames);
    RunScripts(animation_frames, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(animation_frames.title == "number");
    HtmlDoc promises;
    ParseHtml("<script>var order = 'sync'; Promise.resolve(2).then(function(value) { order += ':' + value; return value + 1; }).then(function(value) { order += ':' + value; return Promise.reject('no'); }).catch(function(reason) { order += ':' + reason; return Promise.all([Promise.resolve(4), 5]); }).then(function(values) { return Promise.race([values[0], 9]); }).then(function(value) { return Promise.allSettled([Promise.resolve(value), Promise.reject('bad')]); }).finally(function() { order += ':final'; }).then(function(values) { document.title = order + ':' + values[0].value + ':' + values[0].status + ':' + values[1].reason; });</script>", promises);
    RunScripts(promises, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(promises.title == "sync:2:3:no:final:4:fulfilled:bad");
    HtmlDoc async_functions;
    ParseHtml("<script>async function increment() { var value = await new Promise(function(resolve) { queueMicrotask(function() { resolve(6); }); }); return value + 1; } increment().then(function(value) { document.title = 'async:' + value; });</script>", async_functions);
    RunScripts(async_functions, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(async_functions.title == "async:7");
    HtmlDoc events;
    ParseHtml("<div id='outer'><button id='button'></button></div><script>var log = ''; var outer = document.getElementById('outer'); var button = document.getElementById('button'); outer.addEventListener('go', function(event) { log += 'capture:'; }, true); button.addEventListener('go', function(event) { log += 'target:'; event.preventDefault(); }); button.addEventListener('go', function(event) { log += 'once:'; }, {once: true}); outer.addEventListener('go', function(event) { log += 'bubble'; }); var first = button.dispatchEvent(new Event('go', {bubbles: true, cancelable: true})); button.dispatchEvent(new Event('go', {bubbles: true})); document.title = log + ':' + first;</script>", events);
    RunScripts(events, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(events.title == "capture:target:once:bubblecapture:target:bubble:false");
    HtmlDoc custom_events;
    ParseHtml("<div id='target'></div><script>var target = document.getElementById('target'); target.addEventListener('message', function(event) { document.title = event.detail.text; }); target.dispatchEvent(new CustomEvent('message', {detail: {text: 'received'}}));</script>", custom_events);
    RunScripts(custom_events, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(custom_events.title == "received");
    HtmlDoc pointer_events;
    ParseHtml("<style>button:hover { color: blue; } button:active { background: red; }</style><button id='button'></button><script>var button = document.getElementById('button'); button.addEventListener('mouseover', function(event) { document.title = event.clientX + ':' + event.ctrlKey; }); button.dispatchEvent(new MouseEvent('mouseover', {clientX: 12, ctrlKey: true})); button.dispatchEvent(new MouseEvent('mousedown'));</script>", pointer_events);
    RunScripts(pointer_events, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    DomNode *pointer_button = FindById(pointer_events.root.get(), "button");
    CHECK(pointer_button && pointer_button->interaction_hover && pointer_button->interaction_active && pointer_button->interaction_focus && pointer_events.title == "12:true");
    HtmlDoc keyboard_events;
    ParseHtml("<input id='field'><script>var field = document.getElementById('field'); field.addEventListener('keydown', function(event) { document.title = event.key + ':' + event.code + ':' + event.shiftKey + ':' + event.altKey; }); field.dispatchEvent(new KeyboardEvent('keydown', {key: 'A', code: 'KeyA', shiftKey: true, altKey: true}));</script>", keyboard_events);
    RunScripts(keyboard_events, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(keyboard_events.title == "A:KeyA:true:true");
    HtmlDoc propagation_events;
    ParseHtml("<div id='outer'><button id='button'></button></div><script>var log = ''; var outer = document.getElementById('outer'); var button = document.getElementById('button'); function removed(event) { log += 'removed'; } button.addEventListener('go', removed); button.removeEventListener('go', removed); button.addEventListener('go', function(event) { log += 'first'; event.stopImmediatePropagation(); }); button.addEventListener('go', function(event) { log += 'second'; }); outer.addEventListener('go', function(event) { log += 'outer'; }); button.dispatchEvent(new Event('go', {bubbles: true})); document.title = log;</script>", propagation_events);
    RunScripts(propagation_events, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(propagation_events.title == "first");
    HtmlDoc property_events;
    ParseHtml("<button id='button'></button><script>var button = document.getElementById('button'); button.onclick = function(event) { document.title = event.currentTarget.tagName; }; button.dispatchEvent(new MouseEvent('click'));</script>", property_events);
    RunScripts(property_events, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
    CHECK(property_events.title == "BUTTON");
    HtmlDoc shared_scripts;
    ParseHtml("<script>var shared = 'global'; function suffix() { return shared + '-scope'; }</script><script>document.title = suffix();</script>", shared_scripts);
    RunScripts(shared_scripts, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\n", error.c_str()); std::abort(); });
    CHECK(shared_scripts.title == "global-scope");
    {
        const std::filesystem::path resource_dir = std::filesystem::temp_directory_path() / "mep_html_resource_test";
        std::filesystem::create_directories(resource_dir);
        { std::ofstream css(resource_dir / "page.css"); css << "#styled { color: red; }"; }
        { std::ofstream script(resource_dir / "external.js"); script << "trace += ':external';"; }
        { std::ofstream json(resource_dir / "data.json"); json << "{\"name\":\"resource\",\"count\":3}"; }
        HtmlDoc local_resources;
        ParseHtml("<link rel='stylesheet' href='page.css'><p id='styled'>text</p><script>var trace = 'inline';</script><script src='external.js'></script><script>document.title = trace;</script>", local_resources);
        LoadLocalHtmlResources(local_resources, resource_dir.string());
        RunScripts(local_resources, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
        DomNode *styled = FindById(local_resources.root.get(), "styled");
        CHECK(styled && styled->style.has_color && styled->style.color_r == 0xdc && local_resources.title == "inline:external");
        HtmlDoc fetched_resource;
        ParseHtml("<script>var content_type = ''; fetch('data.json').then(function(response) { content_type = response.headers.entries()[0][1]; return response.json(); }).then(function(value) { document.title = content_type + ':' + value.name + ':' + value.count; });</script>", fetched_resource);
        LoadLocalHtmlResources(fetched_resource, resource_dir.string());
        RunScripts(fetched_resource, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
        CHECK(fetched_resource.title == "application/json:resource:3");
        HtmlDoc blocked_fetch;
        ParseHtml("<script>fetch('../outside.json').catch(function(error) { document.title = 'blocked'; });</script>", blocked_fetch);
        LoadLocalHtmlResources(blocked_fetch, resource_dir.string());
        RunScripts(blocked_fetch, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
        CHECK(blocked_fetch.title == "blocked");
        HtmlDoc missing_fetch;
        ParseHtml("<script>fetch('missing.json').then(function(response) { document.title = response.ok + ':' + response.status + ':' + response.statusText; });</script>", missing_fetch);
        LoadLocalHtmlResources(missing_fetch, resource_dir.string());
        RunScripts(missing_fetch, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
        CHECK(missing_fetch.title == "false:404:Not Found");
        HtmlDoc xhr_resource;
        ParseHtml("<script>var request = new XMLHttpRequest(); request.open('GET', 'data.json'); request.responseType = 'json'; request.onload = function() { document.title = request.status + ':' + request.response.name + ':' + request.readyState; }; request.send();</script>", xhr_resource);
        LoadLocalHtmlResources(xhr_resource, resource_dir.string());
        RunScripts(xhr_resource, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
        CHECK(xhr_resource.title == "200:resource:4");
        HtmlDoc aborted_xhr;
        ParseHtml("<script>var request = new XMLHttpRequest(); request.onabort = function() { document.title = request.readyState + ':' + request.status; }; request.abort();</script>", aborted_xhr);
        LoadLocalHtmlResources(aborted_xhr, resource_dir.string());
        RunScripts(aborted_xhr, [](const std::string &) {}, [](const std::string &error) { std::fprintf(stderr, "%s\\n", error.c_str()); std::abort(); });
        CHECK(aborted_xhr.title == "0:0");
        std::filesystem::remove_all(resource_dir);
    }
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
    CHECK(host && host->shadow_root && host->shadow_root->children.size() == 1 && host->shadow_root->children[0]->tag == "slot" && shadow.title == "SLOT");
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
