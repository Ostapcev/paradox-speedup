# Копирует автосейвы TEST5000 на 1 января каждого N-го года в BENCH<год>.sav (ротация автосейвов
# их не трогает) и пишет прогресс в лог. Останавливается, когда сохранён BENCH<StopYear>.sav
# или игра закрыта.
param(
    [string]$Dir = "$env:USERPROFILE\Documents\Paradox Interactive\Stellaris\save games\2_-26809668",
    [int]$Every = 10,
    [int]$StopYear = 2300,
    [string]$Log = "$PSScriptRoot\..\long_run.log"
)
function Note($text) { "$(Get-Date -Format 'HH:mm:ss') $text" | Tee-Object -FilePath $Log -Append }
Note "watching $Dir, every $Every years, stop at $StopYear"
while ($true) {
    foreach ($f in Get-ChildItem $Dir -Filter 'autosave_*.01.01.sav' -ErrorAction SilentlyContinue) {
        if ($f.Name -match '^autosave_(\d{4})\.01\.01\.sav$') {
            $year = [int]$Matches[1]
            $dst = Join-Path $Dir "BENCH$year.sav"
            if ($year % $Every -eq 0 -and -not (Test-Path $dst)) {
                Start-Sleep 5   # дать игре дописать файл
                Copy-Item $f.FullName $dst
                Note "saved BENCH$year.sav ($([math]::Round($f.Length / 1MB, 1)) MB)"
                if ($year -ge $StopYear) { Note "reached $StopYear"; exit 0 }
            }
        }
    }
    if (-not (Get-Process stellaris -ErrorAction SilentlyContinue)) { Note "game is not running"; exit 1 }
    Start-Sleep 20
}
