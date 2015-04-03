#!mruby

v = "foo"
th = Thread.new(v) do |x|
  puts "in thread: x is #{x}"
  puts "in thread: v is #{v.inspect}" # should be nil
  v = "bar" # should be updated
  "baz"
end

r = th.join
puts "in main: v is #{v}" # should be bar
puts "in main: r is #{r}" # should be remained as baz
