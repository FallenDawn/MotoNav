$ErrorActionPreference='Stop'
$taskJava=if($env:JAVA_HOME){Join-Path $env:JAVA_HOME 'bin/java.exe'}else{'java'}
$taskJavac=if($env:JAVA_HOME){Join-Path $env:JAVA_HOME 'bin/javac.exe'}else{'javac'}
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskClasses=Join-Path $taskRoot '.local/test-classes'
$taskSource=Join-Path $taskRoot 'app/src/main/java/com/example/motonav'
New-Item -ItemType Directory -Force $taskClasses | Out-Null
$taskCases=@(
    @{Sources=@('CruiseIdleTimer'); Test='CruiseIdleTimerTest'},
    @{Sources=@('MapStylePolicy'); Test='MapStylePolicyTest'},
    @{Sources=@('WeatherArea'); Test='WeatherAreaTest'},
    @{Sources=@('ArrivalPolicy'); Test='ArrivalPolicyTest'},
    @{Sources=@('RideTrackMath','MapLoadStatus'); Test='RideToolsTest'}
)
foreach($taskCase in $taskCases){
    $taskFiles=@($taskCase.Sources | ForEach-Object {Join-Path $taskSource "${_}.java"})
    $taskFiles+=Join-Path $PSScriptRoot ($taskCase.Test+'.java')
    & $taskJavac -encoding UTF-8 -d $taskClasses @taskFiles
    if($LASTEXITCODE -ne 0){throw "Compile failed: $($taskCase.Test)"}
    & $taskJava -cp $taskClasses ('com.example.motonav.'+$taskCase.Test)
    if($LASTEXITCODE -ne 0){throw "Test failed: $($taskCase.Test)"}
}
