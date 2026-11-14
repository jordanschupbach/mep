// Guile Scheme language UI mode (<leader>uu / <leader>lu / :MepLanguageUi in
// a .scm buffer), kBuiltinLanguageUi's Guile consumer, built on
// kBuiltinLanguageUiCommon (mep.language_ui_layout) in the Ruby mode's shape.
// Included by main.cpp only; BuiltinLanguageUiGuile() is the Lua chunk
// (help/guile-mode.org is the user's page).
//
// The console is the stock Guile REPL -- `guile -q -l mep_init.scm`
// (mep.opt.guile_ui_cmd, default {'guile'}; a `;; REPL:` comment replaces it
// via mep.language_ui_cmd). -q leaves ~/.guile out (so no colorized
// printer); the init script turns on (ice-9 readline) when it is installed,
// for line editing and history. It inherits mep's environment:
// GUILE_LOAD_PATH (Guile's own), and MEP_GUILE_PATH and the source file's
// directory, which the init script adds to %load-path, so
// `(use-modules (datamunge))` loads what it would in that project.
//
// The init script defines mep-* procedures in (guile-user) and adds a
// before-read-hook, so the panel files are rewritten (only when they change)
// before each prompt; the poll loop re-reads them on a timer.
//   - Module: (module-map ...) of the REPL's current module -- the bindings
//     the session made, with a procedure's arguments or a value. Clicking a
//     row inspects it.
//   - Modules: the current module, the modules it uses, and the
//     directories added to %load-path.
//   - Inspect: (mep-view 'name) -- the value pretty-printed, its type, a
//     procedure's arguments and documentation.
//   - Help: (mep-help 'name) -- what the session knows (the procedure's
//     arguments, the module it comes from, (procedure-documentation) and
//     `,describe`'s object-documentation, which also covers Guile's own
//     reference), then the docstring `mep-scheme-lsp --doc` reads from the
//     defining module's source on %load-path (for a Rosetta-generated
//     module, the C++ documentation in its procedures' docstrings).
//
// Sending code. <leader>re and <leader>rr never paste into the prompt: they
// write the code to a file in the session directory, padded so its line
// numbers are the source's, and type one (mep-eval-file ...) line, which
// reads it as if from the source file and compiles it form by form in the
// current module -- definitions stay, and errors name the source's lines.
// The REPL prints the last value. The block at the cursor is the top-level
// form around it.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

inline const char* BuiltinLanguageUiGuile() {
    return R"MEPLUA(
mep.opt = mep.opt or {}
mep.opt.guile_ui_cmd = mep.opt.guile_ui_cmd or {'guile'}
mep.opt.guile_ui_info_share = mep.opt.guile_ui_info_share or 0.3
mep.opt.guile_ui_bottom_share = mep.opt.guile_ui_bottom_share or 0.4
mep.opt.guile_ui_bottom_info_share = mep.opt.guile_ui_bottom_info_share or 0.4
mep.language_ui_extensions.scm = mep.language_ui_extensions.scm or 'guile'
mep.language_ui_extensions.ss = mep.language_ui_extensions.ss or 'guile'

-- The console's init script. Lua appends the start call below it:
-- (mep-ui-start DIR LSP '(load path dirs...)).
local mep_guile_ui_init_template = [==[
(use-modules (ice-9 pretty-print) (ice-9 documentation) (ice-9 popen) (ice-9 rdelim) (ice-9 session)
             (system base compile) (srfi srfi-1))

(define mep-ui-dir #f)
(define mep-ui-lsp #f)
(define mep-ui-lsp-usable #t)
(define mep-ui-files '())
(define mep-ui-base-symbols '())
(define mep-ui-base-load-path '())
(define mep-ui-base-uses '())
(define mep-ui-last '())

;; Atomically (write, then rename), so a reader never sees a half-written panel.
(define (mep-ui-write name text)
  (unless (equal? (assoc-ref mep-ui-last name) text)
    (set! mep-ui-last (assoc-set! mep-ui-last name text))
    (let* ((path (string-append mep-ui-dir "/" name))
           (tmp (string-append path "." (number->string (getpid)) ".tmp")))
      (false-if-exception
       (begin
         (call-with-output-file tmp (lambda (p) (display text p)))
         (rename-file tmp path))))))

(define* (mep-ui-short v #:optional (width 60))
  (let* ((s (with-output-to-string (lambda () (write v))))
         (s (string-map (lambda (c) (if (char=? c #\newline) #\space c)) s)))
    (if (> (string-length s) width) (string-append (substring s 0 (- width 3)) "...") s)))

(define (mep-ui-pad s n)
  (if (< (string-length s) n) (string-append s (make-string (- n (string-length s)) #\space)) s))

(define (mep-ui-row name kind detail)
  (string-append (mep-ui-pad name 22) " " (mep-ui-pad kind 10) " " detail))

;; A procedure's argument list, "(f a #:optional b . rest)".
(define (mep-ui-signature name proc)
  (let ((args (false-if-exception (procedure-arguments proc))))
    (if (not args)
        (string-append "(" name " ...)")
        (let ((req (or (assq-ref args 'required) '()))
              (opt (or (assq-ref args 'optional) '()))
              (kw (or (assq-ref args 'keyword) '()))
              (rest (assq-ref args 'rest)))
          (string-append
           "(" name
           (apply string-append (map (lambda (a) (string-append " " (symbol->string a))) req))
           (if (null? opt) ""
               (apply string-append " #:optional"
                      (map (lambda (a) (string-append " " (symbol->string a))) opt)))
           (if (null? kw) ""
               (apply string-append " #:key"
                      (map (lambda (a) (string-append " " (keyword->string (car a)))) kw)))
           (if rest (string-append " . " (symbol->string rest)) "")
           ")")))))

(define (mep-ui-join lines)
  (if (null? lines) "" (string-append (string-join lines "\n") "\n")))

(define (mep-ui-refresh)
  (let ((m (current-module)) (rows '()))
    (module-for-each
     (lambda (sym var)
       (unless (or (memq sym mep-ui-base-symbols)
                   (string-prefix? "mep-" (symbol->string sym))
                   (not (variable-bound? var)))
         (let ((name (symbol->string sym)) (v (variable-ref var)))
           (set! rows
                 (cons (cond ((procedure? v) (mep-ui-row name "procedure" (mep-ui-signature name v)))
                             ((macro? v) (mep-ui-row name "macro" ""))
                             ((module? v) (mep-ui-row name "module" (mep-ui-short (module-name v))))
                             (else (mep-ui-row name "variable" (mep-ui-short v))))
                       rows)))))
     m)
    (mep-ui-write "module.txt" (mep-ui-join (sort rows string<?)))
    (mep-ui-write
     "modules.txt"
     (mep-ui-join
      (append (list (string-append "current " (mep-ui-short (module-name m))))
              (filter-map (lambda (u)
                            (let ((n (module-name u)))
                              (and (not (member n mep-ui-base-uses))
                                   (string-append "uses " (mep-ui-short n)))))
                          (module-uses m))
              (filter-map (lambda (d) (and (not (member d mep-ui-base-load-path)) (string-append "path " d)))
                          %load-path))))))

;; The module whose interface gives the current module `sym`: its name, or #f.
(define (mep-ui-owner sym)
  (let ((m (current-module)))
    (if (module-local-variable m sym)
        (module-name m)
        (let loop ((uses (module-uses m)))
          (cond ((null? uses) #f)
                ((module-variable (car uses) sym) (module-name (car uses)))
                (else (loop (cdr uses))))))))

(define (mep-ui-lsp-doc name module)
  (if (or (not mep-ui-lsp-usable) (not mep-ui-lsp) (string-null? mep-ui-lsp))
      ""
      (let* ((args (append (list mep-ui-lsp "--doc" name)
                           (if module (list "--module" (with-output-to-string (lambda () (write module)))) '())
                           (append-map (lambda (f) (list "--file" f)) mep-ui-files)
                           (append-map (lambda (d) (list "-L" d)) %load-path)))
             (port (false-if-exception (apply open-pipe* OPEN_READ args))))
        (if (not port)
            (begin (set! mep-ui-lsp-usable #f) "")
            (let* ((out (read-delimited "" port)) (status (close-pipe port)))
              (cond ((eof-object? out) "")
                    ((eqv? (status:exit-val status) 0) (string-trim-right out))
                    (else (when (not (memv (status:exit-val status) '(1))) (set! mep-ui-lsp-usable #f)) "")))))))

(define (mep-ui-name x)
  (cond ((symbol? x) (symbol->string x)) ((string? x) x) (else (mep-ui-short x))))

(define (mep-ui-help-text x)
  (let* ((name (mep-ui-name x))
         (sym (string->symbol name))
         (var (module-variable (current-module) sym))
         (v (and var (variable-bound? var) (variable-ref var)))
         (owner (mep-ui-owner sym))
         (out '()))
    (define (add . strs) (set! out (append out strs)))
    (cond
     ((procedure? v)
      (add (mep-ui-signature name v) "\n\n")
      (add "procedure" (if owner (string-append " in module " (mep-ui-short owner)) "") "\n")
      (let ((doc (or (false-if-exception (object-documentation v)) (procedure-documentation v))))
        (when (and doc (not (string-null? doc))) (add "\n" doc "\n"))))
     ((macro? v) (add name ": macro" (if owner (string-append " in module " (mep-ui-short owner)) "") "\n"))
     (var (add name ": variable" (if owner (string-append " in module " (mep-ui-short owner)) "") "\n\n"
               (with-output-to-string (lambda () (pretty-print v))))))
    ;; The source's docstring (mep-scheme-lsp prints: signature, docstring, `kind -- file:line`).
    (let* ((src (mep-ui-lsp-doc name (and owner (not (equal? owner '(guile-user))) owner)))
           (lines (string-split src #\newline))
           (live (and (procedure? v) (procedure-documentation v))))
      (cond ((string-null? src) #f)
            ((null? out) (add src "\n"))
            ((and live (string-contains src (string-trim-both live)))
             (add "\nsource: " (let ((l (last lines))) (or (and=> (string-contains l " -- ") (lambda (i) (substring l (+ i 4)))) l)) "\n"))
            (else (add "\n## Source\n\n" src "\n"))))
    (if (null? out)
        (string-append name ": no documentation found in this session.\n")
        (apply string-append out))))

;; `name`'s documentation in mep's Help tab.
(define (mep-help x)
  (mep-ui-write "help.txt"
                (catch #t (lambda () (mep-ui-help-text x))
                  (lambda (key . args) (format #f "~a: ~a ~s\n" (mep-ui-name x) key args))))
  (format #t ";; ~a: in the Help tab\n" (mep-ui-name x))
  (if #f #f))

;; A value (or a binding's name, as a symbol or string) in mep's Inspect tab.
(define (mep-view x)
  (let* ((by-name (and (or (symbol? x) (string? x))
                       (module-variable (current-module) (if (symbol? x) x (string->symbol x)))))
         (v (if (and by-name (variable-bound? by-name)) (variable-ref by-name) x))
         (label (if by-name (mep-ui-name x) (mep-ui-short x)))
         (type (cond ((procedure? v) "procedure") ((pair? v) (if (list? v) "list" "pair")) ((null? v) "empty list")
                     ((vector? v) "vector") ((string? v) "string") ((symbol? v) "symbol") ((number? v) "number")
                     ((hash-table? v) "hash table") ((boolean? v) "boolean") ((char? v) "character")
                     ((record? v) (format #f "record ~a" (record-type-name (record-type-descriptor v))))
                     (else "object")))
         (size (cond ((list? v) (format #f ", ~a elements" (length v))) ((vector? v) (format #f ", ~a elements" (vector-length v)))
                     ((string? v) (format #f ", ~a chars" (string-length v)))
                     ((hash-table? v) (format #f ", ~a entries" (hash-count (const #t) v))) (else "")))
         (out (string-append label "  (" type size ")\n\n" (with-output-to-string (lambda () (pretty-print v))))))
    (when (procedure? v)
      (set! out (string-append out "\n" (mep-ui-signature label v) "\n"))
      (let ((doc (procedure-documentation v))) (when doc (set! out (string-append out "\n" doc "\n")))))
    (mep-ui-write "inspect.txt" out)
    (if #f #f)))

;; `copy` (the source's code, padded to its line numbers), read as if from
;; `src` and compiled form by form in the current module; the last value is
;; the REPL's to print.
(define (mep-eval-file copy src)
  (unless (member src mep-ui-files)
    (when (file-exists? src) (set! mep-ui-files (cons src mep-ui-files))))
  (unless (member copy mep-ui-files) (set! mep-ui-files (cons copy mep-ui-files)))
  (dynamic-wind
    (lambda () #f)
    (lambda ()
      (call-with-input-file copy
        (lambda (p)
          (set-port-filename! p src)
          (let loop ((v (if #f #f)))
            (let ((form (read-syntax p)))
              (if (eof-object? form)
                  v
                  (loop (compile form #:env (current-module) #:from 'scheme #:to 'value))))))))
    (lambda () (false-if-exception (mep-ui-refresh)))))

(define (mep-run copy src)
  (let ((dir (dirname src)))
    (unless (member dir %load-path) (add-to-load-path dir)))
  (mep-eval-file copy src))

(define (mep-ui-start dir lsp paths)
  (set! mep-ui-dir dir)
  (set! mep-ui-lsp lsp)
  (set! mep-ui-base-load-path %load-path)
  (let ((env (getenv "MEP_GUILE_PATH")))
    (for-each (lambda (d) (unless (or (string-null? d) (member d %load-path)) (add-to-load-path d)))
              (append (reverse paths) (if env (reverse (string-split env #\:)) '()))))
  ;; Files the session loads from source (load, a module without a compiled .go).
  (let ((prev %load-hook))
    (set! %load-hook
          (lambda (file)
            (when (and (string? file) (not (member file mep-ui-files)) (not (string-prefix? (%library-dir) file))
                       (not (string-contains file "/share/guile/")))
              (set! mep-ui-files (cons file mep-ui-files)))
            (when (procedure? prev) (prev file)))))
  (false-if-exception
   (begin
     (module-use! (current-module) (resolve-interface '(ice-9 readline)))
     ((module-ref (current-module) 'activate-readline))))
  (set! mep-ui-base-symbols (module-map (lambda (sym var) sym) (current-module)))
  (set! mep-ui-base-uses (map module-name (module-uses (current-module))))
  (add-hook! before-read-hook (lambda () (false-if-exception (mep-ui-refresh))))
  (mep-ui-refresh)
  (format #t "Guile ~a -- mep language UI: <leader>re runs the form at the cursor, <leader>rr the file; (mep-help 'name) / gh shows help, (mep-view 'x) inspects.\n" (version)))
]==]

-- tab id -> session state. The sidebars are single global instances that
-- always render whichever tab is active, as in the other modes.
local state = {}
local sidebars = nil

local function ensure_sidebars()
  if sidebars then return end
  sidebars = {
    module = mep.sidebar_create('Module', 'right', 44),
    modules = mep.sidebar_create('Modules', 'right', 44),
    inspect = mep.sidebar_create('Inspect', 'right', 44),
    help = mep.sidebar_create('Help', 'right', 44),
  }
end

-- A Scheme string literal.
local function scmquote(s)
  return '"' .. tostring(s):gsub('\\', '\\\\'):gsub('"', '\\"') .. '"'
end
mep.guile_ui_quote = scmquote

local function send(st, text)
  mep.terminal_write(st.console_buf, text .. '\n')
end

local function render_module()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.module_text) or ''):gmatch('[^\n]+') do
    local name = line:match('^(%S+)')
    widgets[#widgets + 1] = {id = line, text = line, tooltip = name and ('inspect ' .. name) or nil,
      on_click = (name and st) and function() send(st, '(mep-view ' .. scmquote(name) .. ')') end or nil}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(nothing defined in this module yet)'} end
  mep.sidebar_set_sections(sidebars.module, {{id = 'module', title = '', collapsed = false, widgets = widgets}})
end

local function render_modules()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  local uses, dirs = {}, {}
  local current = ''
  for line in ((st and st.modules_text) or ''):gmatch('[^\n]+') do
    local kind, rest = line:match('^(%S+)%s+(.+)$')
    if kind == 'current' then current = rest
    elseif kind == 'uses' then uses[#uses + 1] = {id = line, text = rest}
    elseif kind == 'path' then dirs[#dirs + 1] = {id = line, text = rest} end
  end
  if #uses == 0 then uses[1] = {id = 'empty', text = '(no modules used yet -- (use-modules (a b)))'} end
  if #dirs == 0 then dirs[1] = {id = 'empty', text = '(nothing added -- (add-to-load-path "dir"))'} end
  mep.sidebar_set_sections(sidebars.modules, {
    {id = 'uses', title = 'Used by ' .. current, collapsed = false, widgets = uses},
    {id = 'path', title = 'Added to %load-path', collapsed = false, widgets = dirs},
  })
end

local function render_all()
  if not sidebars then return end
  local st = state[mep.current_tab_id()]
  render_module()
  render_modules()
  mep.language_ui_render_textbox(sidebars.inspect, st and st.inspect_text,
    "(nothing inspected yet -- click a Module row, or (mep-view 'x) in the console)")
  mep.language_ui_render_textbox(sidebars.help, st and st.help_text,
    "(no help viewed yet -- gh over a name, or (mep-help 'name) in the console)")
end

-- The rows each top-level form of `lines` spans: {first, last} pairs.
-- Strings, characters (#\( ), line comments, #| |# block comments and
-- #! !# are skipped.
local function top_level_forms(lines)
  local forms = {}
  local depth, in_string, block = 0, false, 0
  local start = nil
  local in_atom = false
  for row, l in ipairs(lines) do
    local i, n = 1, #l
    while i <= n do
      local c = l:sub(i, i)
      if in_string then
        if c == '\\' then i = i + 1 elseif c == '"' then in_string = false end
        i = i + 1
      elseif block > 0 then
        if l:sub(i, i + 1) == '|#' or l:sub(i, i + 1) == '!#' then block = block - 1 i = i + 2
        elseif l:sub(i, i + 1) == '#|' then block = block + 1 i = i + 2
        else i = i + 1 end
      elseif c == ';' then
        break
      elseif l:sub(i, i + 1) == '#|' or (l:sub(i, i + 1) == '#!' and i == 1 and row == 1) then
        block = block + 1
        i = i + 2
      elseif c == '"' then
        if depth == 0 and not start then start = row end
        in_string = true
        i = i + 1
      elseif l:sub(i, i + 1) == '#\\' then
        if depth == 0 and not start then start = row end
        i = i + 3
        while i <= n and l:sub(i, i):match('[%w%-]') do i = i + 1 end
        if depth == 0 then forms[#forms + 1] = {start, row} start = nil end
      elseif c == '(' or c == '[' then
        if depth == 0 and not start then start = row end
        depth = depth + 1
        i = i + 1
      elseif c == ')' or c == ']' then
        depth = math.max(0, depth - 1)
        if depth == 0 and start then forms[#forms + 1] = {start, row} start = nil end
        i = i + 1
      elseif c:match('%s') then
        if in_atom and depth == 0 and start then forms[#forms + 1] = {start, row} start = nil end
        in_atom = false
        i = i + 1
      else
        -- An atom, or a quote / #-prefix before a form.
        if depth == 0 and not start then start = row end
        if not c:match("['`,@#]") then in_atom = true end
        i = i + 1
      end
    end
    if in_atom and depth == 0 and start and not in_string then forms[#forms + 1] = {start, row} start = nil end
    in_atom = false
  end
  if start then forms[#forms + 1] = {start, #lines} end
  return forms
end

-- The top-level form(s) around `row`: the first and last rows of every
-- form whose rows include it, or nil between forms.
function mep.guile_ui_block_at(lines, row)
  local s, e = nil, nil
  for _, f in ipairs(top_level_forms(lines)) do
    if f[1] <= row and row <= f[2] then
      s = s and math.min(s, f[1]) or f[1]
      e = e and math.max(e, f[2]) or f[2]
    end
  end
  return s, e
end

-- Writes rows first..last of `lines` to the session's eval file, padded so
-- its line numbers are the source's, and has the console evaluate it.
local function eval_text(st, lines, first, last)
  local chunk = {}
  for i = first, last do chunk[#chunk + 1] = lines[i] end
  local text = string.rep('\n', first - 1) .. table.concat(chunk, '\n') .. '\n'
  mep.language_ui_write_file(st.eval_path, text)
  local fname = mep.filename and mep.filename()
  local name = (fname and fname ~= '') and mep_lsp_abspath(fname) or st.eval_path
  send(st, '(mep-eval-file ' .. scmquote(st.eval_path) .. ' ' .. scmquote(name) .. ')')
end

-- <leader>re: the Visual selection if one is live, else the top-level form
-- at the cursor.
function mep.guile_ui_eval_block()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No Guile console in this tab -- <leader>uu opens one', 'warn')
    return
  end
  local lines = {}
  for i = 1, mep.line_count() do lines[i] = mep.get_line(i) or '' end
  local sel = mep.visual_selection and mep.visual_selection() or ''
  if sel ~= '' then
    local r = mep.visual_range()
    mep.cmd('normal!')
    if r and r.kind == 'char' then
      local sel_lines = {}
      for l in (sel .. '\n'):gmatch('([^\n]*)\n') do sel_lines[#sel_lines + 1] = l end
      local padded = {}
      for i = 1, r.start_row - 1 do padded[i] = '' end
      for i, l in ipairs(sel_lines) do padded[r.start_row - 1 + i] = l end
      eval_text(st, padded, r.start_row, r.start_row + #sel_lines - 1)
    elseif r then
      eval_text(st, lines, r.start_row, r.end_row)
    end
    return
  end
  local s, e = mep.guile_ui_block_at(lines, (mep.cursor()))
  if not s then mep.notify('No Scheme form at the cursor', 'warn') return end
  eval_text(st, lines, s, e)
  if s ~= e then mep.notify(string.format('Sent lines %d-%d to the console', s, e)) end
end
mep.guile_ui_eval_selection = mep.guile_ui_eval_block
mep.language_ui_eval_handlers.guile = mep.guile_ui_eval_block

function mep.guile_ui_open()
  local session_dir = mep.language_ui_session_dir('guileui')
  local paths = {
    module = session_dir .. '/module.txt', modules = session_dir .. '/modules.txt',
    inspect = session_dir .. '/inspect.txt', help = session_dir .. '/help.txt',
  }
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local fname = mep.filename and mep.filename() or ''
  local src_abs = (fname ~= '') and mep_lsp_abspath(fname) or ''
  local src_dir = src_abs:match('^(.*)[/\\][^/\\]*$')
  local add = {}
  if src_dir and src_dir ~= '' then add[#add + 1] = scmquote(src_dir) end
  local lsp = mep.bundled_tool and mep.bundled_tool('mep-scheme-lsp') or 'mep-scheme-lsp'
  local init_path = session_dir .. '/mep_init.scm'
  mep.language_ui_write_file(init_path, mep_guile_ui_init_template .. '\n(mep-ui-start ' .. scmquote(session_dir) .. ' '
    .. scmquote(lsp) .. " '(" .. table.concat(add, ' ') .. '))\n')
  local src_buf = mep.current_buffer()
  local argv = mep.language_ui_cmd(mep.opt.guile_ui_cmd)
  for _, a in ipairs({'-q', '-l', init_path}) do argv[#argv + 1] = a end
  local layout = mep.language_ui_layout({
    info_share = mep.opt.guile_ui_info_share, bottom_share = mep.opt.guile_ui_bottom_share,
    bottom_info_share = mep.opt.guile_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'guile') end,
  })
  local tid = mep.current_tab_id()
  ensure_sidebars()
  local st = {
    paths = paths, dir = session_dir, console_buf = layout.console_buf, editor_pane = layout.editor_pane,
    src_buf = src_buf, src_abs = src_abs, argv = argv,
    eval_path = session_dir .. '/eval.scm', run_path = session_dir .. '/run.scm',
    module_text = '', modules_text = '', inspect_text = '', help_text = '', help_buf = nil,
  }
  state[tid] = st
  render_all()
  mep.language_ui_open_tabs(layout.top_pane, {sidebars.module, sidebars.modules, sidebars.inspect})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sidebars.help})
  st.help_buf = help_bufs[sidebars.help]
  mep.pane_focus(layout.editor_pane)
  mep.notify('Guile language UI: guile REPL below; Module/Modules/Inspect top-right, Help bottom-right; <leader>re runs the form at the cursor, <leader>rr the file')
  -- run_source: <leader>rr / the Run button while the mode is open (a file
  -- with a CMD: line runs that instead, as without the mode) -- the source
  -- buffer as it is now (unsaved edits too), read as if from the file, its
  -- directory on %load-path.
  local function run_source(f)
    local abs = mep_lsp_abspath(f or fname)
    local lines = mep.buffer_get_lines(st.src_buf)
    if not lines then
      send(st, '(mep-run ' .. scmquote(abs) .. ' ' .. scmquote(abs) .. ')')
      return
    end
    mep.language_ui_write_file(st.run_path, table.concat(lines, '\n') .. '\n')
    send(st, '(mep-run ' .. scmquote(st.run_path) .. ' ' .. scmquote(abs) .. ')')
  end
  return {
    run_source = run_source,
    close = function()
      state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('Guile language UI mode closed')
    end,
  }
end
mep.language_ui_modes.guile = {open = mep.guile_ui_open}

-- gh: the session's documentation for the name under the cursor in the Help
-- tab, and from a .scm buffer the language server's hover as well.
function mep.guile_ui_help_at_cursor()
  local st = state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor("[%w_!%$%%&%*/:<=>%?%^~%+%-%.@]")
  if not sym or sym == '' then mep.notify('gh: no Scheme name under the cursor', 'warn') return true end
  send(st, '(mep-help ' .. scmquote(sym) .. ')')
  if mep.language_for_buffer() == 'guile' then
    if st.help_buf then mep.reveal_buffer(st.help_buf, true) end
    if mep.lsp_hover then mep.lsp_hover() end
  elseif st.help_buf then
    mep.jump_to_buffer(st.help_buf)
  end
  return true
end
mep.help_register_provider('guile', mep.guile_ui_help_at_cursor)

-- This tab's session as the panels see it (nil when no Guile mode is open).
function mep.guile_ui_info()
  local st = state[mep.current_tab_id()]
  if not st then return nil end
  return {directory = st.dir, console_buf = st.console_buf, argv = st.argv, module = st.module_text,
    modules = st.modules_text, inspect = st.inspect_text, help = st.help_text}
end

-- Poll loop: re-reads the panel files on a timer, only while a Guile mode
-- is open on the active tab.
function mep.guile_ui_poll(st)
  st = st or state[mep.current_tab_id()]
  if not st then return end
  local changed, help_changed = false, false
  for _, key in ipairs({'module', 'modules', 'inspect', 'help'}) do
    local text = mep.language_ui_read_file(st.paths[key])
    if text and text ~= st[key .. '_text'] then
      st[key .. '_text'] = text
      changed = true
      if key == 'help' then help_changed = true end
    end
  end
  if changed then render_all() end
  if help_changed and st.help_buf then mep.reveal_buffer(st.help_buf, true) end
end
do
  local last_poll = 0
  mep.on_frame(function()
    local st = state[mep.current_tab_id()]
    if not st then return end
    local now = mep.now()
    if now - last_poll < mep.opt.language_ui_poll_interval then return end
    last_poll = now
    mep.guile_ui_poll(st)
  end)
end
)MEPLUA";
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
