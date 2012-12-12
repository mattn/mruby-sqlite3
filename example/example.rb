#!mruby
begin
  SQLite3::Database.new('example/foo.db').execute('create table foo(id integer primary key, text text)')
rescue RuntimeError
end

db = SQLite3::Database.new('example/foo.db')
db.execute('select * from foo') do |row, fields|
  puts row
end

row = db.execute('select * from foo where id = ?', 1)
puts row.fields()
puts row.next()
row.close()

db.close()
