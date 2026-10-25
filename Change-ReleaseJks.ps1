param([string]$JavaPath='')
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$project=Join-Path $root 'android'
$keystoreDir=Join-Path $project 'keystore'
$keyfile=Join-Path $keystoreDir 'personal.jks'
$signing=Join-Path $keystoreDir 'keystore.properties'
$backupDir=Join-Path $root ('output\keystore-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))

Write-Host 'This will replace the release signing JKS and change the release SHA1.' -ForegroundColor Yellow
$answer=Read-Host 'Continue? Type YES to continue'
if($answer -cne 'YES'){ Write-Host 'Cancelled.'; exit 0 }

$javaCandidates=@()
if($JavaPath){ $javaCandidates += $JavaPath }
if($env:JAVA_HOME){ $javaCandidates += $env:JAVA_HOME }
$javaCandidates += @(Get-ChildItem -Path (Join-Path $root '.tools/jdk/*') -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
$javaCandidates += @(Get-PSDrive -PSProvider FileSystem | ForEach-Object {
    $driveRoot=$_.Root
    Get-ChildItem -Path (Join-Path $driveRoot 'Program Files/Java/*') -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
    Get-ChildItem -Path (Join-Path $driveRoot 'Program Files (x86)/Java/*') -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
})
$javaCommand=Get-Command java.exe -ErrorAction SilentlyContinue
if($javaCommand){
    $javaCandidates += (Split-Path (Split-Path $javaCommand.Source))
}
$keytool=$null
foreach($candidate in ($javaCandidates | Select-Object -Unique | Sort-Object @{Expression={ if($_ -match '\\jre|\\jre[0-9]'){1}else{0} }}, @{Expression={ $_ }})){
    $candidateKeytool=Join-Path $candidate 'bin/keytool.exe'
    if(Test-Path $candidateKeytool){ $JavaPath=$candidate; $keytool=$candidateKeytool; break }
}
if(-not $keytool){ throw 'JDK with keytool.exe not found. Run the build once first, set JAVA_HOME to a full JDK, or pass -JavaPath C:\path\to\jdk.' }

New-Item -ItemType Directory -Force $keystoreDir | Out-Null
if((Test-Path $keyfile) -or (Test-Path $signing)){
    New-Item -ItemType Directory -Force $backupDir | Out-Null
    if(Test-Path $keyfile){ Copy-Item $keyfile (Join-Path $backupDir 'personal.jks') }
    if(Test-Path $signing){ Copy-Item $signing (Join-Path $backupDir 'keystore.properties') }
    Write-Host "旧签名已备份到 $backupDir" -ForegroundColor Cyan
}

$password=[Guid]::NewGuid().ToString('N')+[Guid]::NewGuid().ToString('N')
$env:MOTONAV_SIGN_PASSWORD=$password
Remove-Item $keyfile -Force -ErrorAction SilentlyContinue
& $keytool -genkeypair -storetype JKS -keystore $keyfile -storepass:env MOTONAV_SIGN_PASSWORD -keypass:env MOTONAV_SIGN_PASSWORD -alias motonav -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=MotoNav Personal,O=MotoNav,C=CN'
if($LASTEXITCODE -ne 0){ throw 'New signing certificate generation failed.' }
[IO.File]::WriteAllText($signing,"storeFile=personal.jks`nstorePassword=$password`nkeyAlias=motonav`nkeyPassword=$password`n")

$cert=Join-Path $root 'output\certificate-new.der'
New-Item -ItemType Directory -Force (Split-Path $cert) | Out-Null
& $keytool -exportcert -keystore $keyfile -storepass:env MOTONAV_SIGN_PASSWORD -alias motonav -file $cert
if($LASTEXITCODE -ne 0){ throw 'Certificate export failed.' }
$fingerprint=(Get-FileHash -LiteralPath $cert -Algorithm SHA1).Hash -replace '(..)(?!$)','$1:'
$info=Join-Path $root 'output\New-release-signing-info.txt'
[IO.File]::WriteAllText($info,"Package: com.fallendawn.motonav`r`nSHA1: $fingerprint`r`nBackup: $backupDir`r`n")
$env:MOTONAV_SIGN_PASSWORD=$null
Write-Host "New JKS created: $keyfile" -ForegroundColor Green
Write-Host "New release SHA1: $fingerprint" -ForegroundColor Green
Write-Host "Details: $info"
Write-Host 'Back up android/keystore and do not share it.' -ForegroundColor Yellow
