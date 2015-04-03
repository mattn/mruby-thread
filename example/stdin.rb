#require 'mruby-thread'

puts (Thread.new { STDIN.readline }).join
