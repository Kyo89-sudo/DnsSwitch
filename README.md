# DnsSwitch

Piccolo tool Win32 (C++17, nessuna dipendenza) per cambiare, testare e ripristinare i DNS su Windows, sulla falsariga di DNS Jumper.

## Funzioni

- **Schede di rete**: elenca le schede attive con i DNS correnti e indica se sono manuali o automatici (IPv4 e IPv6).
- **Test velocità**: invia query DNS reali (UDP/53, 4 query per server, 8 server in parallelo) e ordina l'elenco per latenza, colorando i risultati. Alla fine seleziona il più veloce.
- **Applica DNS**: imposta IPv4 primario/secondario e, se la scheda ha IPv6, anche gli IPv6. Se il profilo non ha IPv6, l'IPv6 torna automatico, così non restano DNS manuali di un altro provider. Dopo l'applicazione svuota la cache.
- **Voce "Automatico (DHCP / router)"**: in cima all'elenco mostra i DNS che la scheda riceve dal router (letti da `DhcpNameServer`, quindi visibili anche quando i DNS sono impostati a mano; se mancano, usa il gateway). Si può testare come gli altri server; applicarla rimette la scheda su automatico invece di scrivere gli indirizzi come manuali.
- **Ripristina automatico**: riporta IPv4 e IPv6 della scheda ai DNS forniti dal DHCP.
- **Svuota cache DNS**: `DnsFlushResolverCache`, con `ipconfig /flushdns` come ripiego.
- **Modifica elenco…**: apre `servers.txt` nel Blocco note; dopo il salvataggio basta premere "Aggiorna".

Clic sull'intestazione "Nome" o "Latenza" per ordinare. I campi in basso si riempiono selezionando un server, ma si possono anche scrivere a mano per usare DNS personalizzati.

## Elenco server

`servers.txt` sta accanto all'exe e viene creato al primo avvio con un elenco predefinito (Cloudflare, Google, Quad9, OpenDNS, AdGuard, Control D, CleanBrowsing, DNS4EU, Comodo, Level3). Formato:

```
Nome;IPv4 primario;IPv4 secondario;IPv6 primario;IPv6 secondario
```

I campi vuoti sono ammessi e le righe con `#` sono commenti.

## Come funziona il cambio DNS

- Windows 10 2004 e successivi: `SetInterfaceDnsSettings` (iphlpapi), caricata dinamicamente.
- Versioni precedenti: ripiego automatico su `netsh interface ipv4/ipv6 set|add dnsservers`.

Il programma richiede i privilegi di amministratore (manifest `requireAdministrator`).

## Compilazione

**CMake (MSVC o MinGW)**

```
cmake -B build -A x64
cmake --build build --config Release
```

**MSVC rapido**: da "x64 Native Tools Command Prompt for VS" lanciare `build.bat` (output in `build\DnsSwitch.exe`).

**MinGW-w64**

```
windres -I res res/app.rc -O coff -o app.o
g++ -std=c++17 -O2 src/main.cpp app.o -o DnsSwitch.exe -mwindows -municode -static -s ^
    -lws2_32 -liphlpapi -lcomctl32 -ladvapi32 -lshell32 -luxtheme
```
