const standardJsUnity = require('jsunity/jsunity2.js').jsUnity;

const jsUnityRanzWrapper = require('jsunity');
const jsUnity = jsUnityRanzWrapper.jsUnity = standardJsUnity;

const fs = require('fs');
const internal = require('internal');

let suites = [];

jsUnityRanzWrapper.run = function(suite) {
    suites.push(suite);
}
jsUnityRanzWrapper.done = function() {}

module.exports = function(logFile, fileRestrictions, testMatcher) {
    suites = [];
    jsUnity.tap.write = function(line) {
        fs.append(logFile, line + '\n');
    }

    let findfilesRecursive = (files, path) => {
        if (path == 'js/server/tests/stress') {
            return files;
        }
        if (path == 'js/server/tests/dump') {
            return files;
        }
        //console.error("AHA", path);
        if (fs.isDirectory(path)) {
            return fs.list(path)
                .map(basename => fs.join(path, basename))
                .reduce(findfilesRecursive, files);
        } else if (fs.isFile(path) && path.substr(-3) == '.js') {
            if (!path.match(/spec/) && !path.match(/-cluster/) && !path.match(/replication/) && !path.match(/export-setup/) && !path.match(/deadlock/) && !path.match(/stress/)) {
                files.push(path);
            }
        }
        return files;
    }

    let path = 'js/server/tests';
    let files = [];
    if (!Array.isArray(fileRestrictions) || fileRestrictions.length == 0) {
        fileRestrictions = ['js/server/tests', 'js/common/tests'];
    }
    files = fileRestrictions.reduce(findfilesRecursive, files);

    let readFiles = files.map(file => {
        let source = fs.read(file);
        return `(function() { ${source} }());`
    })

    let test = `
(function() {
    ${readFiles.join('\n')}
})();
    (function(suites) {
        let jsUnity = require('./jsunity/jsunity2').jsUnity;
        jsUnity.attachAssertions();
        jsUnity.run(suites, ${JSON.stringify(testMatcher)});
    })
`;

    let f = internal.executeScript(test, undefined, '(none)');
    f(suites);
    console.log("Done testing");
    return true;
}