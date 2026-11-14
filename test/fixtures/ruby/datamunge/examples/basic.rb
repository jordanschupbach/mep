# CMD: ruby -I ../lib basic.rb
require 'datamunge'

frame = Datamunge::Frame.read_csv("data.csv")
col = frame.column(0)
puts col.sum
puts Datamunge.mean(col.to_a)
