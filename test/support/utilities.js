var fs = require('fs');
var util = require('util');
var path = require('path');
var spawn = require('child_process').spawn;

exports.imageEqualsFile = function(buffer, file, callback) {
    file = path.resolve(file);
    var compare = spawn('compare', ['-metric', 'PSNR', '-', file, '/dev/null' ]);

    var error = '';
    compare.stderr.on('data', function(data) {
        error += data.toString();
    });
    compare.on('exit', function(code, signal) {
        if (code) return callback(new Error(error || 'Exited with code ' + code));
        else if (error.trim() === 'inf') callback(null);
        else {
            var similarity = parseFloat(error.trim());
            var type = path.extname(file);
            var result = path.join(path.dirname(file), path.basename(file, type) + '.result' + type);
            fs.writeFileSync(result, buffer);
            var err = new Error('Images not equal (' + similarity + '): ' + result);
            err.similarity = similarity;
            callback(err);
        }
    });

    compare.stdin.write(buffer);
    compare.stdin.end();
};
