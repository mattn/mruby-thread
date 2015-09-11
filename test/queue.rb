##
# Queue test

assert('Object.const_defined? :Queue') do
  Object.const_defined?(:Queue)
end

assert('Queue#empty? ') do
  q = Queue.new
  assert_equal q.empty?, true
end

assert('Queue#push and #shift') do
  q = Queue.new
  q.push 1
  q.push 2
  q.push 3
  assert_equal q.shift, 1
  assert_equal q.shift, 2
  assert_equal q.shift, 3
end

assert('Queue#clear') do
  q = Queue.new
  q.push 42
  q.clear
  q.empty?
end

assert('Queue#push and #pop') do
  q = Queue.new
  q.push 1
  q.push 2
  q.push 3
  assert_equal q.pop, 3
  assert_equal q.pop, 2
  assert_equal q.pop, 1
end

assert('Queue#enq and #deq') do
  q = Queue.new
  q.enq 1
  q.enq 2
  q.enq 3
  assert_equal q.deq, 1
  assert_equal q.deq, 2
  assert_equal q.deq, 3
end

assert('Queue#size') do
  q = Queue.new
  assert_equal q.size, 0
  q.enq 42
  assert_equal q.size, 1
  q.deq
  assert_equal q.size, 0
end


