#!mruby

sql = 'create table foo(id integer primary key, text text);'

db = SQLite3::Database.new('example/foo.db')
begin
  db.execute_batch(sql)
rescue RuntimeError
end

db.execute_batch('delete from foo')
db.execute_batch('insert into foo(text) values(?)', 'foo')
db.execute_batch('insert into foo(text) values(?)', 'bar')
db.transaction()
db.execute_batch('insert into foo(text) values(?)', 'baz')
db.rollback
db.transaction()
db.execute_batch('insert into foo(text) values(?)', 'bazoooo!')
db.commit

db.execute('select * from foo') do |row, fields|
  puts row
end

row = db.execute('select * from foo where text = ?', 'foo')
puts row.fields()
while !row.eof?
  puts row.next()
end
row.close()

db.close()
