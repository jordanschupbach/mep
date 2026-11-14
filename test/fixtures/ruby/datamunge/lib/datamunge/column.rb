# frozen_string_literal: true

module Datamunge
  # One column of a Frame.
  class Column < NativeObject
    # The values.
    # @return [Array<Float>]
    def to_a; end

    # The sum of the values.
    # @return [Float]
    def sum; end

    # Statistics helpers, usable as module functions.
    module Stats
      module_function

      # The variance of +values+.
      # @param values [Array<Float>] the samples
      # @param ddof [Integer] delta degrees of freedom
      # @return [Float]
      def variance(values, ddof = 1); end
    end
  end
end
