# frozen_string_literal: true

ENV['MRUBY_CONFIG']  ||= File.expand_path('build_config.rb')
ENV['MRUBY_VERSION'] ||= 'stable'

file :mruby do
  case ENV['MRUBY_VERSION']&.downcase
  when 'head', 'master'
    sh(*%w[git clone --depth 1 git://github.com/mruby/mruby.git])
  when 'stable'
    sh(*%w[git clone --depth 1 git://github.com/mruby/mruby.git -b stable])
  else
    sh "curl -L --fail --retry 3 --retry-delay 1 https://github.com/mruby/mruby/archive/#{ENV['MRUBY_VERSION']}.tar.gz -s -o - | tar zxf -" # rubocop:disable Layout/LineLength
    mv "mruby-#{ENV['MRUBY_VERSION']}", 'mruby'
  end
end

task default: 'test'

desc 'compile binary'
task compile: 'mruby' do
  sh(*%w[rake -f mruby/Rakefile all])
end

desc 'run mtests'
task test: 'mruby' do
  sh(*%w[rake -f mruby/Rakefile test])
end

desc 'cleanup target build folder'
task :clean do
  sh(*%w[rake -f mruby/Rakefile clean]) if Dir.exist? 'mruby'
end

desc 'cleanup all build folders'
task :cleanall do
  sh(*%w[rake -f mruby/Rakefile deep_clean]) if Dir.exist? 'mruby'
end
