class MyContainer
  attr_accessor :data
  def initialize(d = nil)
    @data = d
    super
  end
end

$mutex = Mutex.new
$queue = MyContainer.new(1)
$ary = []

svr = Thread.new($queue, $mutex, $ary) do |q, m, a|
  20.times do |i|
    q.data = q.data + 1
    a << q.data
    a.shift if a.size > 3
    Thread.sleep 2
  end
end

while true do
  puts "count = #{$queue.data}, ary = #{$ary}"
  if $queue.data > 10 then
    svr.kill
    break
  end
  Thread.sleep 1
end

