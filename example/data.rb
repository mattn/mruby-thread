class MyContainer
  attr_accessor :data
  def initialize(d = nil)
    @data = d
    super
  end
end

$mutex = Mutex.new
$queue = MyContainer.new(1)

svr = Thread.new($queue, $mutex) do |q, m|
  20.times do |i|
    q.data = q.data + 1
    Thread.sleep 1
  end
end

while true do
  puts "count = #{$queue.data}"
  if $queue.data > 10 then
    svr.kill
    break
  end
  Thread.sleep 1
end

