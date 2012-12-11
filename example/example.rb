#!mruby

db = SQLite3::Database.new('example/foo.db')

db.execute('select * from foo') do |row, fields|
  puts row
end

row = db.execute('select * from foo where id = ?', 1)
puts row.fields()
puts row.next()

db.close()
