MRuby::Gem::Specification.new('mruby-thread') do |spec|
  spec.license = 'MIT'
  spec.authors = 'mattn'

  defs = %w[MRB_THREAD_COPY_VALUES MRB_USE_ETEXT_EDATA]
  build.cc.defines += defs
  spec.cc.defines+= defs

  if build.toolchains.include?("androideabi")
    spec.cc.flags << '-DHAVE_PTHREADS'
  else
    spec.linker.libraries << ['pthread']
  end
end
