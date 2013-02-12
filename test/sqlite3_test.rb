assert('create database') do
  db = SQLite3::Database.new('mruby-sqlite-test.db')
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
  db.close
  true
end

assert('execute batch') do
  db = SQLite3::Database.new('mruby-sqlite-test.db')
  db.execute_batch('delete from foo')
  db.execute_batch('insert into foo(text) values(?)', 'foo')
  db.execute_batch('insert into foo(text) values(?)', 'bar')
  db.close
  true
end

assert('transaction short') do
  db = SQLite3::Database.new('mruby-sqlite-test.db')
  db.transaction
  db.execute_batch('insert into foo(text) values(?)', 'baz')
  db.rollback
  db.transaction
  db.execute_batch('insert into foo(text) values(?)', 'bazoooo!')
  db.commit
  db.close
  true
end

assert('transaction long') do
  db = SQLite3::Database.new('mruby-sqlite-test.db')
  db.transaction
  (1..100).each_with_index {|x,i|
    db.execute_batch('insert into bar(id, text) values(?, ?)', x, x)
  }
  db.commit
  n = 1
  s = 0
  db.execute('select id from bar') do |row, fields|
    s += 1 if row[0] == n
    n += 1
  end
  db.close
  s == 100
end
