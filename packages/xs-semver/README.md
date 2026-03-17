# xs-semver

```xs
import semver

semver.parse("1.2.3-rc.1+build.5")
// #{major: 1, minor: 2, patch: 3, prerelease: "rc.1", build: "build.5"}

semver.compare("1.2.3", "1.2.10")    // -1
semver.satisfies("1.2.5", "^1.2.3")  // true
semver.satisfies("2.0.0", "^1.2.3")  // false
semver.satisfies("1.2.5", "~1.2.3")  // true
```
