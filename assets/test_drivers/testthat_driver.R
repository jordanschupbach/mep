# mep's Tests panel driver for R (src/main.cpp's testthat test provider).
#
#   Rscript testthat_driver.R list ROOT
#   Rscript testthat_driver.R run ROOT [SPEC ...]
#
# A test is one test_that() block, named FILE::DESCRIPTION, so the panel can
# show each file as a group of its tests; one inside describe() is named
# FILE::OUTER / INNER the way testthat itself reports it, and the panel
# nests it under OUTER. `list` finds them by parsing
# tests/testthat/test-*.R (no code runs). `run` takes SPECs that are either a
# whole FILE or one FILE::DESCRIPTION (run through test_file's `desc`
# filter), every test-*.R file when there are none. Both report in the
# protocol src/test_runner.h parses:
#
#   @@mep-test case FILE::DESCRIPTION
#   @@mep-test start FILE::DESCRIPTION
#   @@mep-test result STATUS SECONDS FILE::DESCRIPTION
#
# with what failed in a test (or why it was skipped) between its start and
# result lines. A file whose code fails outside any test_that() reports
# that under FILE::(file) instead.
#
# A package (ROOT/DESCRIPTION) is loaded once with pkgload::load_all, so its
# internal functions are visible to its tests the way devtools::test() has
# them.

mark <- function(...) cat("\n@@mep-test ", ..., "\n", sep = "")

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 2 || !(args[[1]] %in% c("list", "run"))) {
  cat("usage: Rscript testthat_driver.R list|run ROOT [SPEC ...]\n")
  quit(status = 2)
}
mode <- args[[1]]
root <- normalizePath(args[[2]])
specs <- args[-(1:2)]
testdir <- file.path(root, "tests", "testthat")
all_files <- list.files(testdir, pattern = "^test.*\\.[rR]$")

# The literal descriptions of every test_that()/it() call in a file,
# wherever it sits (top level, or inside describe()/local()/a loop body),
# with the describe() blocks around it in front: "outer / inner". A test
# whose description is computed (paste(...)) is not listed here; it shows up
# in the panel once a run reports it.
test_names <- function(file) {
  exprs <- tryCatch(parse(file.path(testdir, file), keep.source = FALSE), error = function(e) NULL)
  found <- character()
  fn_name <- function(fn) {
    if (is.name(fn)) return(as.character(fn))
    if (is.call(fn) && identical(as.character(fn[[1]]), "::")) return(as.character(fn[[3]]))
    ""
  }
  walk <- function(e, outer) {
    if (!is.call(e)) return(invisible())
    fn <- fn_name(e[[1]])
    literal <- length(e) >= 2 && is.character(e[[2]]) && length(e[[2]]) == 1
    if (fn %in% c("test_that", "it") && literal) {
      found[[length(found) + 1]] <<- paste(c(outer, e[[2]]), collapse = " / ")
      return(invisible())
    }
    if (fn == "describe" && literal) {
      for (part in as.list(e)[-(1:2)]) if (!missing(part)) walk(part, c(outer, e[[2]]))
      return(invisible())
    }
    for (part in as.list(e)[-1]) if (!missing(part)) walk(part, outer)
  }
  for (e in exprs) walk(e, character())
  unique(found)
}

if (mode == "list") {
  for (f in all_files) for (d in test_names(f)) mark("case ", f, "::", d)
  quit(status = 0)
}

if (length(specs) == 0) specs <- all_files
# FILE -> NULL (the whole file) or the descriptions asked for.
plan <- list()
for (s in specs) {
  at <- regexpr("::", s, fixed = TRUE)
  if (at < 0) {
    plan[s] <- list(NULL)
  } else {
    f <- substr(s, 1, at - 1)
    d <- substr(s, at + 2, nchar(s))
    if (!(f %in% names(plan)) || !is.null(plan[[f]])) plan[[f]] <- c(plan[[f]], d)
  }
}

fail_all <- function(f, descs, why) {
  for (d in (if (is.null(descs)) test_names(f) else descs)) {
    mark("start ", f, "::", d)
    cat(why, "\n", sep = "")
    mark("result failed 0 ", f, "::", d)
  }
}

if (!requireNamespace("testthat", quietly = TRUE)) {
  for (f in names(plan)) fail_all(f, plan[[f]], "testthat is not installed (install.packages('testthat'))")
  quit(status = 1)
}

load_error <- NULL
if (file.exists(file.path(root, "DESCRIPTION"))) {
  if (requireNamespace("pkgload", quietly = TRUE)) {
    load_error <- tryCatch({
      suppressMessages(pkgload::load_all(root, export_all = TRUE, helpers = FALSE, quiet = TRUE))
      NULL
    }, error = function(e) conditionMessage(e))
  } else {
    load_error <- "pkgload is not installed, so the package cannot be loaded for its tests"
  }
}

# One expectation, as the panel shows it under a failed or skipped test.
describe_expectation <- function(exp) {
  where <- ""
  if (!is.null(exp$srcref)) where <- sprintf(" (%s:%d)", basename(attr(exp$srcref, "srcfile")$filename %||% ""), exp$srcref[[1]])
  kind <- if (inherits(exp, "expectation_error")) "Error" else if (inherits(exp, "expectation_skip")) "Skipped" else "Failure"
  paste0("── ", kind, where, " ──\n", conditionMessage(exp))
}
`%||%` <- function(a, b) if (is.null(a)) b else a

any_failed <- FALSE
# test_file()'s results, one entry per test (as.data.frame() would lose an
# error's expectation). A describe() block is an entry of its own, with no
# expectations, ahead of or after its tests; it is the group, not a test.
report <- function(f, res) {
  names <- vapply(res, function(x) x$test, "")
  for (x in res) {
    name <- x$test
    exps <- x$results
    if (length(exps) == 0 && any(startsWith(names, paste0(name, " / ")))) next
    if (!nzchar(name) || grepl("^\\(code run outside", name)) name <- "(file)"
    mark("start ", f, "::", name)
    bad <- any(vapply(exps, function(e) inherits(e, c("expectation_failure", "expectation_error")), TRUE))
    skipped <- length(exps) > 0 && all(vapply(exps, function(e) inherits(e, "expectation_skip"), TRUE))
    status <- if (bad) "failed" else if (skipped) "skipped" else "passed"
    if (bad) any_failed <<- TRUE
    for (exp in exps) {
      if (inherits(exp, c("expectation_failure", "expectation_error", "expectation_skip"))) cat(describe_expectation(exp), "\n")
    }
    mark("result ", status, " ", sprintf("%.3f", x$real), " ", f, "::", name)
  }
}

run_file <- function(f, desc = NULL) {
  out <- NULL
  res <- tryCatch(
    testthat::test_file(file.path(testdir, f), reporter = testthat::SilentReporter$new(),
                        desc = if (is.null(desc)) NULL else strsplit(desc, " / ", fixed = TRUE)[[1]],
                        stop_on_failure = FALSE),
    error = function(e) {
      out <<- conditionMessage(e)
      NULL
    })
  if (is.null(res)) {
    any_failed <<- TRUE
    fail_all(f, if (is.null(desc)) NULL else desc, out)
  } else {
    report(f, res)
  }
}

for (f in names(plan)) {
  if (!is.null(load_error)) {
    any_failed <- TRUE
    fail_all(f, plan[[f]], paste0("could not load the package: ", load_error))
  } else if (is.null(plan[[f]])) {
    run_file(f)
  } else {
    for (d in plan[[f]]) run_file(f, d)
  }
}
quit(status = if (any_failed) 1 else 0)
