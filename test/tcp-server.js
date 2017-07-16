const net = require('net');
const fs = require('fs');
const crypto = require('crypto');

var server = net.createServer();  
server.on('connection', handleConnection);

server.listen(8123, function() {  
  console.log('server listening to %j', server.address());
});

function handleConnection(conn) {  
  var remoteAddress = conn.remoteAddress + ':' + conn.remotePort;
  console.log('new client connection from %s', remoteAddress);

  conn.on('data', onConnData);
  conn.once('close', onConnClose);
  conn.on('error', onConnError);

  function onConnData(d) {
    console.log('connection data from %s: %s', remoteAddress, d);
    const fileStats = fs.statSync('app.bin');
    fs.readFile('app.bin' , (err, data) =>{
     if(!err){
      var sizeBuf = new Buffer(4);
      sizeBuf.writeUInt32LE(fileStats.size)
      conn.write(sizeBuf);

      sha1_for_file = crypto.createHash('sha1');
      sha1_for_file.update(data);
      conn.write(sha1_for_file.digest())
      
      conn.write(data);
     }
     else {
      console.log('readfile error: %s',err.message);
     }
    });
  }

  function onConnClose() {
    console.log('connection from %s closed', remoteAddress);
  }

  function onConnError(err) {
    console.log('Connection %s error: %s', remoteAddress, err.message);
  }
}
