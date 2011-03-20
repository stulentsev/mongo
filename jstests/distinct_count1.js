
t = db.distinctCount1;
t.drop();

assert.eq( 0 , t.distinct( "a" ).length , "test empty" );

t.save( { a : 1 } )
t.save( { a : 2 } )
t.save( { a : 2 } )
t.save( { a : 2 } )
t.save( { a : 3 } )
t.save( { a : 4 } )
t.save( { a : 4 } )

// normal form
res = db.runCommand({distinct : 'distinctCount1', key : 'a', {}, count : true});
assert.eq( "4" , res.count.toString() , "A1" );

// value of count != true, ignore it and fall back to default behaviour
res = db.runCommand({distinct : 'distinctCount1', key : "a" , { a : { $lt : 3 } }, count : 2 } );
assert.eq( "1,2" , res.values , "A2" );
assert.eq( undefined, res.count, 'A3' );

res = db.runCommand({distinct : 'distinctCount1', key : "a" , { a : { $lt : 3 } }, count : "true" } );
assert.eq( "1,2" , res.values , "A4" );
assert.eq( undefined, res.count, 'A5' );

t.drop();

t.save( { a : { b : "a" } , c : 12 } );
t.save( { a : { b : "b" } , c : 12 } );
t.save( { a : { b : "c" } , c : 12 } );
t.save( { a : { b : "c" } , c : 12 } );

res = t.distinct( "a.b" );
assert.eq( "a,b,c" , res.toString() , "B1" );
