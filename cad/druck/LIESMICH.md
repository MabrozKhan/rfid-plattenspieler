# Druckdateien

Alle Teile sind fertig ausgerichtet und liegen mit der richtigen Seite auf dem Druckbett. Einfach in Bambu Studio laden.

Sortiert nach Farbe. 01 bis 10 sind grau, dazu gehören auch alle Teile, die innen sitzen und die man nicht sieht. 11 ist das Namensschild mit Farbwechsel. Ab 12 ist alles schwarz.

| Datei | Farbe | Support | Hinweis |
| --- | --- | --- | --- |
| 01_Oberteil_grau | grau | nein | liegt kopfüber, Deckseite auf dem Bett, Brim |
| 02_Bodenplatte_grau | grau | ja, nur unter dem Motorhalter | Brim |
| 03_Lagerdeckel_grau | grau | nein | trägt den RC522, wird lose auf den Ring gesetzt |
| 04_Lagerstift_grau | grau | nein | wird in den Lagerdeckel geklebt |
| 05_Riemenrad_grau | grau | nein | sitzt unter dem Teller, unsichtbar |
| 06_Tonarm_Anschlagring_grau | grau | nein | sitzt unter dem Deckel |
| 07_Bedienhalter_Taster_Encoder_grau | grau | nein | wird von unten an die zwei Dome der Taster geschraubt |
| 08_Shuffle_Halter_grau | grau | nein | nimmt den 12x12-Taster auf, wird unter den Deckel geklebt |
| 09_Shuffle_Stoessel_grau | grau | nein | wird von unten in den Puck geklebt, verschwindet unter dem Puck |
| 10_Bohrschablone_Shuffle_grau | grau | nein | Hilfsteil zum Bohren, kommt nicht ins Gerät |
| 11_Namensschild_grau_ab_1-0mm_schwarz | grau, ab 1,0 mm schwarz | nein | grau bis 1,0 mm, dann schwarz (AMS), Rückseite auf dem Bett, Schichthöhe 0,08 |
| 12_Teller_schwarz | schwarz | ja, innen im Kragen | Bügeln für die Oberseite |
| 13_Schallplatte_schwarz | schwarz | nein | liegt mit der Oberseite auf dem Bett |
| 14_Tonarm_Saeule_schwarz | schwarz | ja, unter der Scheibe |  |
| 15_Tonarm_Arm_schwarz_PAUSE_8-6mm | schwarz | nein | Pause bei 8,6 mm, M8-Mutter einlegen |
| 16_Tonarm_Achse_Stift_schwarz | schwarz | nein | zwei Stärken, den passenden nehmen |
| 17_Tonarm_Stuetze_schwarz | schwarz | nein |  |
| 18_Drehknopf_schwarz | schwarz | nein | Bügeln, Schichthöhe 0,08 |
| 19_Tastenkappe_Zurueck_links_schwarz | schwarz | nein | linkes Loch, Symbol vertieft, Schichthöhe 0,08 |
| 19_Tastenkappe_Weiter_rechts_schwarz | schwarz | nein | rechtes Loch, Symbol vertieft, Schichthöhe 0,08 |
| 20_Shuffle_Puck_schwarz | schwarz | nein | Shuffle-Knopf hinten links, Symbol vertieft, kein Bügeln |
| 21_Fuss_1 bis 4_schwarz | schwarz | nein |  |

## Einstellungen

Schichthöhe 0,12 mm für Oberteil und Teller, variable Schichthöhe an, Naht hinten, Außenwand 60 bis 80 mm/s, präzise Wand an. Bügeln nur für die oberste Fläche.

Namensschild mit AMS. In Bambu Studio in der Vorschau den Schieberegler auf die erste Schicht über 1,0 mm stellen, mit Rechtsklick einen Filamentwechsel auf Schwarz setzen. Bis 1,0 mm druckt Grau, das ist später der Grund der Gravur.

## Montage

1. RC522 zuerst unter den Lagerdeckel schrauben (M2). Dann den Deckel auf den Ring in der Bodenplatte setzen, die Nase greift hinten in die Kerbe. Das Kabel läuft innen nach unten zum Fenster hinten im Ring.
2. Lagerstift in den Deckel kleben, Teller aufsetzen.
3. Tonarm-Säule von oben durch den Deckel, Anschlagring von unten auf die Abflachung kleben.
4. Mikroschalter unter dem Deckel auf die zwei Stifte stecken, Hebel nach oben, Hebelende unter das Loch.
5. Im Tonkopf steckt die M8-Mutter. Mehr Gewicht bringt eine M8-Madenschraube (6 mm) oder Lötzinn im Mutterloch.
6. Stütze von unten mit einer M2-Schraube durch den Deckel befestigen.
7. Bedienhalter: Die zwei 12x12-Taster mit den Beinchen in die Schlitze legen, die Beinchen schauen unten raus und werden dort verlötet. Den Drehgeber von unten durch das Loch in der Plattform stecken und mit seiner Mutter oben auf der Plattform festziehen. Dann den Halter mit zwei M2-Schrauben von unten an die Dome neben den Tastenlöchern schrauben. Der Hals des Drehgebers sitzt so 9 mm tiefer als bei der Befestigung direkt am Deckel, der Knopf schwebt knapp über dem Deckel.
8. Alle Schrauben sind M2 und werden direkt in die 1,8-mm-Löcher gedreht.
9. Shuffle-Loch bohren. Die Bohrschablone auf die hintere linke Ecke legen, die Lippen liegen außen an der linken und der hinteren Wand an. Durch die Führung mit 7 mm bohren. Die Mitte liegt 22 mm von links und 22 mm von hinten.
10. Den 12x12-Taster mit den Beinchen in die Schlitze des Shuffle-Halters legen. Den Halter unter dem Loch an den Deckel kleben, der Stößel des Tasters zeigt nach oben zum Loch.
11. Den Stößel von unten durch das Loch stecken und oben in die Buchse des Pucks kleben. Der Flansch unten verhindert, dass er herausfällt. Vor dem Kleben prüfen, dass der Taster klickt.
12. Shuffle-Taster an GPIO 17 und GND (Stecker J8). Linke Tastenkappe ist Zurück (GPIO 33), rechte ist Weiter (GPIO 32).
13. Namensschild unten links an die Front kleben, etwa 12 mm vom linken Rand und 4 mm über der Unterkante. Sekundenkleber in kleinen Punkten auf die Rückseite, sonst quillt er an den Rändern raus.
