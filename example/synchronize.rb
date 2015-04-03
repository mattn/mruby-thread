#require 'mruby-thread'

m = Mutex.new
v = "foo"
th = Thread.new(v, m) do |v, m|
  for num in 1..3 do
    m.synchronize do
      puts "thread: num = #{num}"
    end
    Thread.sleep 1
  end
  nil
end
for num in 1..3 do
  m.synchronize do
    puts "main: num = #{num}"
  end
  Thread.sleep 1
end
th.join
