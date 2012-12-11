#!mruby

db = SQLite3::Database.new('example/foo.db')
db.execute('select * from foo where id = ?', 1) do |row, fields|
  #p fields
  p row
end
db.close()
