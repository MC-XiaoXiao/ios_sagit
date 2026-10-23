module mi6.local/apfs-image

go 1.27.1

require github.com/deploymenttheory/go-apfs-v2 v0.18.0

require (
	github.com/google/uuid v1.6.0 // indirect
	github.com/ulikunitz/xz v0.5.17 // indirect
	golang.org/x/crypto v0.57.0 // indirect
	golang.org/x/sys v0.48.0 // indirect
	howett.net/plist v1.0.1
)

replace github.com/deploymenttheory/go-apfs-v2 => ../src/go-apfs
