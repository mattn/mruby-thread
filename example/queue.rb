#require 'mruby-thread'

q = Queue.new
th = Thread.new(q) do |q|
  q.push 1
  q.push 2
  q.push 3
  nil
end
while true
  Thread.sleep 1
  v = q.shift
  break unless v
  puts v
end
th.join
