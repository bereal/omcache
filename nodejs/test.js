var echo = console.log;
var omc = require('./build/Release/omcache');
var c = new omc.OMCache('localhost:' + process.argv[2],
                       {'timeout': 10000});
/*
c.set('TEST_SET', 'HELLO', 123, function(err, data) {
    console.log('SET TEST_SET, err:', err);
    c.get('TEST_SET', function(err, data) {
        console.log('GOT TEST_SET: ', data);
    });
});
*/
c.set('TEST_INCREMENT', '123', 123, function(err, data) {
    console.log('SET TEST_INCREMENT');
    c.increment('TEST_INCREMENT', 22, function(err, data) {
        console.log('123');
        c.get('TEST_INCREMENT', function(err, data) {
            console.log('GOT TEST_INCREMENT: ', data);
        });
    });
});

//c.set('123', 'def', 123, function(err, data){})

//c.set('123', 'def', 123, function(err, data) {
//    console.log(123);
    /*c.get('abc2', function(err, data) {
        console.log('got', err, data);
    });*/
//})
