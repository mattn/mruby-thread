#require 'mruby-thread'

m = Mutex.new
v = "foo"
th = Thread.new(v, m) do |v, m|
  for num in 1..3 do
    m.lock
    puts "thread: num = #{num}"
    m.unlock
    Thread.sleep 1
  end
  nil
end
for num in 1..3 do
  m.lock
  puts "main: num = #{num}"
  m.unlock
  Thread.sleep 1
end
th.join
