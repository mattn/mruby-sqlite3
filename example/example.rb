#!mruby

sql = 'create table foo(id integer primary key, text text);'

db = SQLite3::Database.new('example/foo.db')
begin
  #db.execute_batch(sql)
rescue RuntimeError
end

db.execute_batch('delete from foo')
db.execute_batch('insert into foo(text) values(?)', 'foo')
db.execute_batch('insert into foo(text) values(?)', 'bar')

db.execute('select * from foo') do |row, fields|
  puts row
end

row = db.execute('select * from foo where text = ?', 'foo')
puts row.fields()
puts row.next()
row.close()

db.close()
