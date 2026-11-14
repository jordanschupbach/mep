# frozen_string_literal: true

# A stand-in for the stub file rosetta's Ruby backend generates for the
# datamunge library: every class and method of the native extension, with
# YARD doc comments. mep's Ruby language server reads it through the load
# path (src/ruby_lsp_test.cpp, tools/ruby_ui_check.py).

require_relative 'datamunge/version'

# Data frames and statistics.
#
# @example
#   require 'datamunge'
#   frame = Datamunge::Frame.read_csv("data.csv")
module Datamunge
  # The golden ratio.
  # @return [Float]
  GOLDEN = 1.618033988749895

  # Computes the arithmetic mean of +values+.
  #
  # @param values [Array<Float>] the samples
  # @param skip_nan [Boolean] ignore NaN entries
  # @return [Float] the mean
  # @raise [ArgumentError] if +values+ is empty
  def self.mean(values, skip_nan: false); end

  # Base class of every wrapped native object.
  class NativeObject
    # Whether the native object is still alive.
    # @return [Boolean]
    def alive?; end
  end

  # A column-oriented data frame.
  class Frame < NativeObject
    include Enumerable

    # @return [Integer] the number of rows
    attr_reader :rows

    # @return [String, nil] the frame's name
    attr_accessor :name

    # Creates a frame with +rows+ empty rows.
    #
    # @param rows [Integer] the initial row count
    # @param name [String, nil] an optional name
    def initialize(rows = 0, name = nil); end

    # Reads a CSV file.
    #
    # @param path [String] the file to read
    # @param sep [String] the field separator
    # @return [Frame] the frame read
    # @raise [IOError] when the file cannot be read
    def self.read_csv(path, sep: ","); end

    # Selects a column.
    #
    # @overload column(index)
    #   By position.
    #   @param index [Integer] the zero-based column index
    #   @return [Column] the column
    # @overload column(name, default = nil)
    #   By name.
    #   @param name [String] the column name
    #   @param default [Column, nil] returned when there is no such column
    #   @return [Column] the column
    def column(*args); end

    # Keeps the rows whose +column+ equals +value+.
    #
    # @param column [String] the column to test
    # @param value [Float, nil] the value to keep (nil keeps every row)
    # @param options [Hash] extra options
    # @option options [Boolean] :strict (false) raise on unknown columns
    # @return [Frame] a new frame
    # @example Keep the rows where x is 1
    #   frame.filter("x", 1.0)
    def filter(column, value = nil, **options); end

    # Yields each row.
    # @yieldparam row [Array<Float>] the row's values
    # @return [self]
    def each(&block); end

    # All the columns.
    # @return [Array<Column>]
    def columns; end
  end
end

require 'datamunge/column'
