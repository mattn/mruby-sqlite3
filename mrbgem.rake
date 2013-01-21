MRuby::Gem::Specification.new('mruby-sqlite3') do |spec|
  spec.license = 'MIT'
  spec.authors = 'mattn'
 
  spec.linker.libraries << 'sqlite3'
end
