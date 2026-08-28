$folders = 'E:\Projects\Weird\TOS\kernel',
           'E:\Projects\Weird\TOS\userspace'

$folders | Get-ChildItem -Recurse


Get-ChildItem -Path "C:\Path1", "C:\Path2" -Recurse
