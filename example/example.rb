#!mruby

db = SQLite3::Database.new('example/foo.db')
begin
  db.execute_batch(
    'drop table foo;' + \
    'drop table bar;' \
  )
rescue RuntimeError
ensure
  db.execute_batch( \
    'create table foo(id integer primary key, text text);' + \
    'create table bar(id integer primary key, text text);' \
  )
end

db.execute_batch('delete from foo')
db.execute_batch('insert into foo(text) values(?)', 'foo')
db.execute_batch('insert into foo(text) values(?)', 'bar')
db.transaction()
db.execute_batch('insert into foo(text) values(?)', 'baz')
db.rollback()
db.transaction()
db.execute_batch('insert into foo(text) values(?)', 'bazoooo!')
db.commit()

db.transaction()
(1..100).each_with_index {|x,i|
  db.execute_batch('insert into bar(id, text) values(?, ?)', i, x)
}
db.commit()

db.execute('select * from foo') do |row, fields|
  puts row
end

puts db.execute('select id from foo where text = ?', 'foo').next()[0]

row = db.execute('select * from bar')
puts row.fields()
while cols = row.next()
  puts cols
end
row.close()

db.close()
