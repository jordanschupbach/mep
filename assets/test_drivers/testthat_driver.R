# mep's Tests panel driver for R (src/main.cpp's testthat test provider).
#
#   Rscript testthat_driver.R ROOT [FILE ...]
#
# Runs tests/testthat/FILE (every test-*.R when none are named) for the
# package or project at ROOT, one file at a time, and reports each in the
# protocol src/test_runner.h parses:
#
#   @@mep-test start FILE
#   @@mep-test result STATUS SECONDS FILE     (passed, failed or skipped)
#
# with the file's own output (testthat's summary of what failed) between.
# A package (ROOT/DESCRIPTION) is loaded once with pkgload::load_all, so its
# internal functions are visible to its tests the way devtools::test() has
# them.

mark <- function(...) cat("\n@@mep-test ", ..., "\n", sep = "")

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) {
  cat("usage: Rscript testthat_driver.R ROOT [FILE ...]\n")
  quit(status = 2)
}
root <- normalizePath(args[[1]])
files <- args[-1]
testdir <- file.path(root, "tests", "testthat")
if (length(files) == 0) files <- list.files(testdir, pattern = "^test.*\\.[rR]$")

if (!requireNamespace("testthat", quietly = TRUE)) {
  for (f in files) {
    mark("start ", f)
    cat("testthat is not installed (install.packages('testthat'))\n")
    mark("result failed 0 ", f)
  }
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

any_failed <- FALSE
for (f in files) {
  mark("start ", f)
  t0 <- proc.time()[["elapsed"]]
  status <- "failed"
  if (!is.null(load_error)) {
    cat("could not load the package: ", load_error, "\n", sep = "")
  } else {
    res <- tryCatch(
      as.data.frame(testthat::test_file(file.path(testdir, f),
                                        reporter = testthat::SummaryReporter$new(show_praise = FALSE),
                                        stop_on_failure = FALSE)),
      error = function(e) {
        cat(conditionMessage(e), "\n")
        NULL
      })
    if (!is.null(res)) {
      bad <- sum(res$failed) + sum(res$error)
      status <- if (bad > 0) "failed" else if (nrow(res) > 0 && all(res$skipped)) "skipped" else "passed"
    }
  }
  if (status == "failed") any_failed <- TRUE
  mark("result ", status, " ", sprintf("%.3f", proc.time()[["elapsed"]] - t0), " ", f)
}
quit(status = if (any_failed) 1 else 0)
