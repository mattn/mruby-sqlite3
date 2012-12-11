#!mruby
db = SQLite3::Database.new('example/foo.db')
begin
  db.execute(db, 'create table foo(id integer primary key, text text)')
rescue RuntimeError
  # TODO: reopen
  db = SQLite3::Database.new('example/foo.db')
end

db.execute('select * from foo') do |row, fields|
  puts row
end

row = db.execute('select * from foo where id = ?', 1)
puts row.fields()
puts row.next()

db.close()
