#!mruby

db = SQLite3::Database.new('example/foo.db')
db.execute('select * from foo') do |row, fields|
  p fields
  p row
end
db.close()
