#include "soundfix.h"
#include "ui_soundfix.h"

#include <QMessageBox>
#include <QDir>
#include <QFileDialog>
#include <QProgressDialog>
#include <QProcess>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QTcpSocket>
#include <QTime>
#include <QThread>
#include <QCloseEvent>
#include <QRadioButton>
#include <QTextDocument>
#include <Phonon>

#include "specpp.h"

#define USE_MIDOMI

#define TEST_IDENT_SRV "localhost"

/*
d bpm ratio
d possibly fix tempo
- de ce iti da bspec_confidence aiurea
- cand cauti "Spanish Harlem Orchestra - Que Bonito" a doua oara ia rezultate aiurea din cache
d best of juan matos (calla buey) ... e modificata melodia dar merge
d crapa cu 'best cuban salsa dancing' si yt din k797DByfmP8.mp4 (old guy)
d da zero peste tot cu mpjw56YFjMo.mp4
d crapa cu best of juan matos kimberli - calla buey
d cache midomi replies, ca sa poti testa totu rapid
d cache youtube search results
d offsets are wrong from 2nd on
d sync issues
d nu iti da eroare daca esueaza ffmpeg-encode-u (si probabil info, etc)
- normalize volume
d utf8 in midomi response
- fa-l sa tina cont de tempo ratio din ui
- tooltips for youtube result urls
- nu merge sa downloadezi bachata e cocolata daca il cauti manual (versiunea daniela blabla)
- schimba labelu cu download progress imediat, sa vada useru ca ai luat click-ul
- api pt specpp comun pt soundfix si specpp, sau lasa-l doar in soundfix
- youtube-dl uses ipv6
- progressing progress bars
d mags ala vad ca nu conteaza la sincronizare (match)
- timeout for midomi
- if offset is negative it just uses 0 probably
d append to log
d it loads localhost for youtube play urls
- update default button after each step
- make progressbars modal (on top)
- handle less than 10 youtube results
- if the test audio ends while you have the save dialog open, the "play" icon disappears from the button
d if flv is complete youtube-dl doesn't show dl location. find(vid.*) ?
d need a test (bpm/strong beats) for when the song doesn't match
- "cannot synchronize audio tracks" appears; progress bar stuck at "loading ... video"
- ramane synchronizing audio tracks... desi se termina si il poti canta/salva
- cleanup when redoing steps
- se blocheaza daca dai download youtube a doua oara dupa ce termina d/l (probabil fixed daca faci cleanup)
d offsetstable->clearcontents leaves the grid for items there
- margin for youtube titles
*/

SoundFix::SoundFix(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::SoundFix),
    radioGroup(new QButtonGroup(this)),
    playingRow(-1)
{
    ui->setupUi(this);

    // identification

    QTime time = QTime::currentTime();
    qsrand((uint)time.msec());

    sock = new QTcpSocket(this);
    connect(sock, SIGNAL(readyRead()), this, SLOT(sockReadyRead()));
    connect(sock, SIGNAL(connected()), this, SLOT(sockConnected()));
    connect(sock, SIGNAL(error(QAbstractSocket::SocketError)),
                 this, SLOT(sockError(QAbstractSocket::SocketError)));

    speexTimer = new QTimer(this);
    connect(speexTimer, SIGNAL(timeout()), this, SLOT(sendSpeexChunk()));

    partners = "%7B%22installed%22%3A%5B%5D%7D";
    loadSession();

    identProgressBar.setCancelButtonText("Cancel");

    // youtube search

    thumbMgr = new QNetworkAccessManager(this);

    ui->youtubeTable->setHorizontalHeaderLabels(
            QStringList() << "Use" << "Play" << "Sample" << "Title");

    ui->youtubeTable->setColumnWidth(0, 0); //30);
    ui->youtubeTable->setColumnWidth(1, 0); //40);
    ui->youtubeTable->setColumnWidth(2, 90);

    //ui->youtubeTable->horizontalHeaderItem(0)->setTextAlignment(Qt::AlignLeft);
    //ui->youtubeTable->horizontalHeaderItem(1)->setTextAlignment(Qt::AlignLeft);
    //ui->youtubeTable->horizontalHeaderItem(2)->setTextAlignment(Qt::AlignLeft);
    ui->youtubeTable->horizontalHeaderItem(3)->setTextAlignment(Qt::AlignLeft);

    // sync

    specpp_init();

    ui->tempoCombo->addItem("0%");

    ui->offsetsTable->setHorizontalHeaderLabels(
            QStringList() << "Test" << "Time offset" << "Confidence");

    ui->offsetsTable->setColumnWidth(0, 40);
    ui->offsetsTable->setColumnWidth(1, 90);

    //ui->offsetsTable->horizontalHeaderItem(0)->setTextAlignment(Qt::AlignLeft);
    ui->offsetsTable->horizontalHeaderItem(1)->setTextAlignment(Qt::AlignLeft);
    ui->offsetsTable->horizontalHeaderItem(2)->setTextAlignment(Qt::AlignLeft);

    player = Phonon::createPlayer(Phonon::MusicCategory, Phonon::MediaSource(""));
    player->setParent(this);

    connect(player, SIGNAL(stateChanged(Phonon::State, Phonon::State)), this,
            SLOT(playerStateChanged(Phonon::State)));

    // test
    QTimer::singleShot(0, this, SLOT(appReady()));
}

QString unaccent(const QString s)
{
    QString s2 = s.normalized(QString::NormalizationForm_D);
    QString out;
    for (int i=0,j=s2.length(); i<j; i++)
        // strip diacritic marks
        if (s2.at(i).category()!=QChar::Mark_NonSpacing &&
            s2.at(i).category()!=QChar::Mark_SpacingCombining)
            out.append(s2.at(i));
    return out;
}

int nop_progress(void *, const char *, int)
{
    return 0;
}

// debug
void SoundFix::appReady()
{
    return;

    if (!specpp_compare(L"../specpp/data/dame.wav", L"../specpp/data/dame2.wav", nop_progress, this,
            //scores
            3, MAX_SYNC_OFFSETS, 75, &retOffsets, offsets, confidences, NULL))
        { error("Audio sync error", "Cannot synchronize audio tracks."); return; }

    return;

    recordingPath = "C:\\Users\\bogdan\\Downloads\\Jose Luis _ Pamela Salsa dancing.mp4";
    ui->videoEdit->setText(recordingPath);
    ui->songEdit->setText("Domenic M - Se�ora");

    //startIdentification();
    getVideoInfo();

    //startYoutubeDown("CU8V4BSuRKI");
    youtubeDownDestination = "VLio_1undiA.flv";
    //runAudioSync();
}

void SoundFix::closeEvent(QCloseEvent *event)
{
    cleanupIdentification();
    cleanupYoutubeSearch();
    cleanupYoutubeDown();

    specpp_cleanup();

    event->accept();
}

void SoundFix::loadSession()
{
    QFile sessionFile("data/session.txt");
    if (!sessionFile.open(QFile::ReadOnly))
        return;

    printf("loading session\n");

    for (;;) {
        QString line = sessionFile.readLine(1024).trimmed();
        if (line.isEmpty()) break;

        int sp = line.indexOf(' ');
        if (sp<0) break;

        QString name = line.left(sp);
        QString value = line.right(line.length() - (sp+1));

        printf("%s is [%s]\n", name.toAscii().data(), value.toAscii().data());

        if      (name == "userAgent") userAgent = value;
        else if (name == "PHPSESSID") phpsessid = value;
        else if (name == "recent_searches_cookie_1") recent_searches = value;
        else if (name == "num_searches_cookie") num_searches = value;
        else if (name == "partners_cookie") partners = value;
        else printf("unknown session var: %s\n", name.toAscii().data());
    }

    sessionFile.close();
}

SoundFix::~SoundFix()
{
    delete ui;
}

void SoundFix::cleanupIdentification()
{
    printf("identification cleanup\n");

    speexTimer->stop();
    speexFile.close();
    sock->abort();

    identProgressBar.setValue(identProgressBar.maximum());
}

void SoundFix::on_browseBtn_clicked()
{
    cleanupIdentification();

    recordingPath = QFileDialog::getOpenFileName(this, "Open Video", QString());
    if (recordingPath.isNull())
        return;

    recordingName = QFileInfo(recordingPath).fileName();

    ui->videoEdit->setText(QDir::toNativeSeparators(recordingPath));

    startIdentification();
}

void SoundFix::error(const QString &title, const QString &text)
{
    QMessageBox::warning(this, title, text, QMessageBox::Ok);
}

void SoundFix::information(const QString &title, const QString &text)
{
    QMessageBox::information(this, title, text, QMessageBox::Ok);
}

enum {
    IDENTIFY_EXTRACT_AUDIO=0,
    IDENTIFY_GET_SESSION,
    IDENTIFY_POST_SAMPLE
};

void SoundFix::startIdentification()
{
    identProgressBar.setMinimum(0);
    identProgressBar.setMaximum(100);
    identProgressBar.setValue(0);
    identProgressBar.setMinimumDuration(100);

    identSubstep = IDENTIFY_EXTRACT_AUDIO;
    continueIdentification();
}

class Thr : public QThread {
    public: static void msleep(unsigned long msecs) { QThread::msleep(msecs); }
};

// TODO if progressBar is canceled stop identification

void SoundFix::continueIdentification()
{
    switch (identSubstep) {
        case IDENTIFY_EXTRACT_AUDIO:
            identProgressBar.setLabelText("Analyzing audio track...");
            identProgressBar.setValue(25);
            QApplication::processEvents();

            if (!extractAudio())
                cleanupIdentification();
            return;

        case IDENTIFY_GET_SESSION:
            identProgressBar.setLabelText("Starting audio identification session...");
            identProgressBar.setValue(50);
            QApplication::processEvents();

            getSession();
            return;

        case IDENTIFY_POST_SAMPLE:
            identProgressBar.setLabelText("Identifying audio track...");
            identProgressBar.setValue(75);
            QApplication::processEvents();

            #ifdef USE_MIDOMI
            Thr::msleep(2500);
            #endif

            postSample();
            return;
    }
}

#define SAMPLE_MSEC (24*1000)

bool SoundFix::getVideoInfo()
{
    printf("getting video info\n");

    QProcess ffmpegInfo;
    ffmpegInfo.start("tools/ffmpeg.exe", QStringList() << "-i" << recordingPath);
    if (!ffmpegInfo.waitForFinished())
        { error("Video load error", "Cannot get video information."); return false; }

    QByteArray info = ffmpegInfo.readAllStandardError();

    // Duration: 00:00:21.38,
    QRegExp re("\n *Duration: ([0-9]+):([0-9]+):([0-9]+).([0-9]+)");
    if (re.indexIn(info) == -1) {
        printf("---\n%s---\n", info);
        error("Video load error", "Cannot get video duration.");
        return false;
    }

    int hours = re.cap(1).toInt();
    int mins  = re.cap(2).toInt();
    int secs  = re.cap(3).toInt();
    int hsecs = re.cap(4).toInt();

    durationMsec = (hours*3600 + mins*60 + secs)*1000 + hsecs*10;
    printf("duration: %d\n", durationMsec);

    return true;
}

bool SoundFix::extractAudio()
{
    if (!getVideoInfo())
        return false;

    if (durationMsec < SAMPLE_MSEC) {
        error("Video is too short",
            QString("Video is too short (%1 seconds). At least %2 seconds are required.")
                     .arg(durationMsec/1000).arg(SAMPLE_MSEC/1000));
        return false;
    }

    int sampleOffset = 0;
    if (durationMsec > SAMPLE_MSEC)
        sampleOffset = (durationMsec - SAMPLE_MSEC) / 2;

    // ---

    printf("extracting sample from offset: %d\n", sampleOffset/1000);

    QFile(QString("data/%1 - sample.wav").arg(recordingName)).remove();
    QFile(QString("data/%1 - sample.ogg").arg(recordingName)).remove();
    QFile(QString("data/%1 - sample.spx").arg(recordingName)).remove();

    // should just chdir to data in processes

    QProcess ffmpegWav;
    ffmpegWav.start("tools/ffmpeg.exe", QStringList() <<
            "-i" << recordingPath <<
            "-f" << "wav" <<
            "-ac" << "1" <<
            "-ar" << "44100" <<
            QString("data/%1 - sample.wav").arg(recordingName));
    if (!ffmpegWav.waitForFinished() || ffmpegWav.exitCode() != 0)
        { error("Audio load error", "Cannot extract audio sample."); return false; }

    QProcess ffmpegOgg;
    ffmpegOgg.start("tools/ffmpeg.exe", QStringList() <<
            "-i" << QString("data/%1 - sample.wav").arg(recordingName) <<
            "-acodec" << "libspeex" <<
            "-ac" << "1" <<
            "-ar" << "8000" <<
            "-ss" << QString::number(sampleOffset/1000) <<
            "-t" << QString::number(SAMPLE_MSEC/1000) <<
            "-cbr_quality" << "10" <<
            "-compression_level" << "10" <<
            QString("data/%1 - sample.ogg").arg(recordingName));
    if (!ffmpegOgg.waitForFinished() || ffmpegOgg.exitCode() != 0)
        { error("Audio conversion error", "Cannot compress audio sample."); return false; }

    // ---

    printf("converting to raw speex\n");

    QFile ogg(QString("data/%1 - sample.ogg").arg(recordingName));
    QFile spx(QString("data/%1 - sample.spx").arg(recordingName));

    if (!ogg.open(QIODevice::ReadOnly))
        { error("Audio sample conversion error", "Cannot open sample."); return false; }
    if (!spx.open(QIODevice::WriteOnly))
        { error("Audio sample conversion error", "Cannot create sample."); return false; }

    bool headers=true;
    bool first=true;

    for (;;) {
        unsigned char ogghdr[27];
        unsigned char segtab[255];
        unsigned char seg[80];

        size_t ret = ogg.read((char*)ogghdr, sizeof(ogghdr));
        if (ret==0) break;

        if (ret != sizeof(ogghdr) || memcmp(ogghdr, "OggS", 4))
            { error("Audio sample conversion error", "Cannot read header."); return false; }

        unsigned char nsegs = ogghdr[26];
        if (nsegs==0)
            { error("Audio sample conversion error", "Cannot read audio segments."); return false; }

        ret = ogg.read((char*)segtab, nsegs);
        if (ret != nsegs)
            { error("Audio sample conversion error", "Cannot read audio segment table."); return false; }

        if (nsegs > 1)
            headers = false;

        if (headers) {
            if (nsegs != 1)
                { error("Audio sample conversion error", "Unsupported audio format."); return false; }

            ret = ogg.read((char*)seg, segtab[0]);
            if (ret != segtab[0])
                { error("Audio sample conversion error", "Error reading audio."); return false; }

            if (first) {
                unsigned char speex_header_bin[] =
                    "\x53\x70\x65\x65\x78\x20\x20\x20\x73\x70\x65\x65\x78\x2d\x31\x2e\x32\x62\x65\x74"
                    "\x61\x33\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x50\x00\x00\x00\x40\x1f\x00\x00"
                    "\x00\x00\x00\x00\x04\x00\x00\x00\x01\x00\x00\x00\xff\xff\xff\xff\xa0\x00\x00\x00"
                    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";

                 if (segtab[0] != 80)
                    { error("Audio sample conversion error", "Unsupported audio format."); return false; }
                 spx.write((char*)speex_header_bin, 80);
            }
        } else {
            for (int s=0; s<nsegs; s++) {
                if (segtab[s] > 62 || (s<nsegs-1 && segtab[s] != 62))
                    { error("Audio sample conversion error", "Unsupported audio frame."); return false; }

                ret = ogg.read((char*)seg, segtab[s]);
                if (ret != segtab[s])
                    { error("Audio sample conversion error", "Cannot read audio frame."); return false; }

                unsigned char lenbuf[2] = {0, segtab[s]};
                spx.write((char*)lenbuf, 2);
                spx.write((char*)seg, segtab[s]);
            }
        }

        first = false;
    }

    ogg.close();
    spx.close();

    if (loadCachedSongName()) {
        cleanupIdentification();
    } else {
        identSubstep++;
        continueIdentification();
    }

    return true;
}

bool SoundFix::loadCachedSongName()
{
    QFile songFile(QString("data/%1 - song.txt").arg(recordingName));
    if (!songFile.open(QFile::ReadOnly))
        return false;

    QTextStream songStream(&songFile);
    songStream.setCodec("UTF-8");
    QString artist = songStream.readLine().trimmed();
    QString track = songStream.readLine().trimmed();
    if (artist.isEmpty() || track.isEmpty())
        return false;

    ui->songEdit->setText(QString("%1 - %2").arg(artist, track));
    return true;
}

void SoundFix::sockConnected()
{
    if (identSubstep == IDENTIFY_GET_SESSION) {
        QString cookies = "partners_cookie=" + partners;

        QString req = QString(
                "GET /v2/?method=getAvailableCharts&from=charts HTTP/1.1\r\n"
                #ifdef USE_MIDOMI
                "Host: api.midomi.com:443\r\n"
                #else
                "Host: " TEST_IDENT_SRV ":80\r\n"
                #endif
                "Connection: Keep-Alive\r\n"
                "Cookie: %1\r\n"
                "Cookie2: $Version=1\r\n"
                "User-Agent: %2\r\n"
                "\r\n").arg(cookies, userAgent);

        contentLength = -1;
        sockBuf.truncate(0);
        sock->write(req.toAscii().data());
    } else if (identSubstep == IDENTIFY_POST_SAMPLE) {
        QString cookies = "partners_cookie=" + partners;
        cookies += "; PHPSESSID=" + phpsessid;

        if (!recent_searches.isEmpty())
            cookies += "; recent_searches_cookie_1=" + recent_searches;

        if (!recent_searches.isEmpty())
            cookies += "; num_searches_cookie=" + num_searches;

        QString req = QString(
                "POST /v2/?method=search&type=identify&url=sh_button&prebuffer=0 HTTP/1.1\r\n"
                #ifdef USE_MIDOMI
                "Host: api.midomi.com:443\r\n"
                "Transfer-Encoding: chunked\r\n"
                #else
                "Host: " TEST_IDENT_SRV ":80\r\n"
                "Content-Length: 0\r\n"
                #endif
                "User-Agent: %1\r\n"
                "Cookie: %2\r\n"
                "\r\n").arg(userAgent, cookies);

        contentLength = -1;
        sockBuf.truncate(0);
        sock->write(req.toAscii().data());

        printf("sending speex data\n");

        #ifdef USE_MIDOMI
        speexTimer->start(2150);
        #endif
    }
}

void SoundFix::sendSpeexChunk()
{
    if (!speexFile.isOpen()) {
        speexFile.setFileName(QString("data/%1 - sample.spx").arg(recordingName));
        if (!speexFile.open(QFile::ReadOnly)) {
            error("Error opening audio sample", "Cannot open audio sample for identification.");
            cleanupIdentification();
            return;
        }
    }

    char burst[5*1024];
    int blen = speexFile.read(burst, sizeof(burst));
    if (blen < 0) blen = 0;

    printf("%d bytes to send\n", blen);
    if (blen==0)
        speexTimer->stop();

    while (blen > 0) {
        int clen = 2*1024;
        if (clen > blen)
            clen = blen;
        sock->write(QString("%1\r\n").arg(clen, 0, 16).toAscii().data());
        sock->write(burst, clen);
        sock->write("\r\n");
        memmove(burst, burst+clen, blen-clen);
        blen -= clen;
    }
}

void SoundFix::sockReadyRead()
{
    if (identSubstep != IDENTIFY_GET_SESSION &&
        identSubstep != IDENTIFY_POST_SAMPLE)
    {
        error("Network error", "Unexpected response from identification service.");
        cleanupIdentification();
        return;
    }

    QByteArray chunk = sock->readAll();
    //printf("received: %d\n", chunk.length());
    sockBuf.append(chunk);

    if (contentLength < 0) {
        int rnrn = sockBuf.indexOf("\r\n\r\n");
        if (rnrn < 0) return;

        if (!sockBuf.startsWith("HTTP/1.0 200 ") &&
            !sockBuf.startsWith("HTTP/1.1 200 "))
        {
            error("Song identification error", "Identification service returned an error.");
            cleanupIdentification();
            return;
        }

        headers = sockBuf.left(rnrn+2);

        int clenPos = headers.indexOf("\r\nContent-Length: ", 0, Qt::CaseInsensitive);
        if (clenPos < 0 || clenPos > rnrn) {
            error("Song identification error", "Invalid response from identification service.");
            cleanupIdentification();
            return;
        }

        clenPos += strlen("\r\nContent-Length: ");
        int rn = headers.indexOf("\r\n", clenPos);
        QString clenStr = headers.mid(clenPos, rn-clenPos);

        contentLength = clenStr.toInt();
        //printf("content-length: %d\n", contentLength);


        sockBuf = sockBuf.right(sockBuf.length() - (rnrn+4));
    }

    //printf("have: %d\n", sockBuf.length());
    if (sockBuf.length() < contentLength)
        return;

    sockBuf.truncate(contentLength);
    printf("---\n%s\n---\n", sockBuf.data());

    if (identSubstep == IDENTIFY_GET_SESSION)
        processSessionResponse();
    else if (identSubstep == IDENTIFY_POST_SAMPLE)
        processSearchResponse();
}

void SoundFix::collectCookies()
{
    int hdrpos = 0;

    for (;;) {
        hdrpos = headers.indexOf("\r\nSet-Cookie: ", hdrpos, Qt::CaseInsensitive);
        if (hdrpos < 0) break;
        hdrpos += strlen("\r\nSet-Cookie: ");

        int prn = headers.indexOf("\r\n", hdrpos);

        int p0 = prn;
        int p1 = headers.indexOf(";", hdrpos);
        int p2 = headers.indexOf(" ", hdrpos);
        if (p1>0 && p1 < p0) p0 = p1;
        if (p2>0 && p2 < p1) p0 = p2;

        int peq = headers.indexOf("=", hdrpos);
        if (peq < 0) break;

        QString name  = headers.mid(hdrpos, peq-hdrpos);
        QString value = headers.mid(peq+1, p0-(peq+1));

        printf("%s is [%s]\n", name.toAscii().data(), value.toAscii().data());

        if (name == "PHPSESSID") phpsessid = value;
        if (name == "recent_searches_cookie_1") recent_searches = value;
        if (name == "num_searches_cookie") num_searches = value;

        hdrpos = prn;
    }

    QFile sessionFile("data/session.txt");
    if (!sessionFile.open(QFile::WriteOnly))
        return;

    sessionFile.write(QString("userAgent %1\n").arg(userAgent).toAscii());
    sessionFile.write(QString("PHPSESSID %1\n").arg(phpsessid).toAscii());
    sessionFile.write(QString("recent_searches_cookie_1 %1\n").arg(recent_searches).toAscii());
    sessionFile.write(QString("num_searches_cookie %1\n").arg(num_searches).toAscii());
    sessionFile.write(QString("partners_cookie %1\n").arg(partners).toAscii());

    sessionFile.close();
}

void SoundFix::processSessionResponse()
{
    collectCookies();

    if (phpsessid.isEmpty())
        { error("Song identification error", "Cannot get identification session."); return; }

    identSubstep++;
    continueIdentification();
    return;
}

void SoundFix::processSearchResponse()
{
    collectCookies();

    cleanupIdentification();

    int trackPos  = sockBuf.indexOf("track_name=\"");
    int artistPos = sockBuf.indexOf("artist_name=\"");
    int trackEnd;
    int artistEnd;

    if (trackPos >= 0) {
        trackPos += strlen("track_name=\"");
        trackEnd = sockBuf.indexOf("\">", trackPos);
    }

    if (artistPos >= 0) {
        artistPos += strlen("artist_name=\"");
        artistEnd = sockBuf.indexOf("\">", artistPos);
    }

    if (trackPos<0 || artistPos<0 || trackEnd<0 || artistEnd<0) {
        error("Song identification not available", "Could not identify this song automatically.");
        return;
    }

    QString track  = QString::fromUtf8(sockBuf.data() + trackPos,  trackEnd-trackPos);
    QString artist = QString::fromUtf8(sockBuf.data() + artistPos, artistEnd-artistPos);

    ui->songEdit->setText(QString("%1 - %2").arg(artist, track));

    QFile songFile(QString("data/%1 - song.txt").arg(recordingName));
    if (songFile.open(QFile::WriteOnly)) {
        QTextStream songStream(&songFile);
        songStream.setCodec("UTF-8");
        songStream << artist << "\n";
        songStream << track << "\n";
    }
}

void SoundFix::sockError(QAbstractSocket::SocketError)
{
    if (identSubstep == IDENTIFY_GET_SESSION)
        error("Network error", "Cannot get song charts information.");
    else if (identSubstep == IDENTIFY_GET_SESSION)
        error("Network error", "Cannot do automatic song identification.");

    cleanupIdentification();
}

void SoundFix::getSession()
{
    if (!phpsessid.isEmpty()) {
        identSubstep++;
        continueIdentification();
        return;
    }

    printf("getting charts\n");

    QString uid;
    for (int i=0; i<16; i++)
        uid.append((qrand()%2) ? ('0' + qrand()%10) : ('a' + qrand()%6));

    userAgent = QString(
        "AppNumber=31 "
        "AppVersion=5.1.7b "
        "APIVersion=2.0.0 "
        "DEV=GT-I9100_GT-I9100 "
        "UID=%1 "
        "FIRMWARE=2.3.4_eng.build.20120314.185218 "
        "LANG=en_US "
        "6IMSI=310260 "
        "COUNTRY=us "
        "NETWORK=WIFI")
        .arg(uid);

    sock->abort();

    #ifdef USE_MIDOMI
    sock->connectToHost("api.midomi.com", 443);
    #else
    sock->connectToHost(TEST_IDENT_SRV, 80);
    #endif
}

void SoundFix::postSample()
{
    printf("posting sample\n");

    sock->abort();

    #ifdef USE_MIDOMI
    sock->connectToHost("search.midomi.com", 443);
    #else
    sock->connectToHost(TEST_IDENT_SRV, 80);
    #endif
}

void SoundFix::on_searchBtn_clicked()
{
    // TODO check that step 1 completed

    QByteArray songName = unaccent(ui->songEdit->text()).toAscii();
    cleanSongName.clear();
    QString songQuery;

    int lc = 0;
    for (int i=0; i<songName.length(); i++) {
        int c = songName[i];
        if ((c < 'a' || c > 'z') &&
            (c < 'A' || c > 'Z') &&
            (c < '0' || c > '9'))
            c = ' ';

        if (c == ' ' && lc == ' ') continue;

        cleanSongName.append(c);
        lc = c;

        int q = (c==' ') ? '+' : c;
        songQuery.append(q);
    }

    cleanSongName = cleanSongName.trimmed();

    ui->songLabel->setText(QString("<a href=\"http://www.youtube.com/results?search_query=%1\">%2</a>")
            .arg(songQuery).arg(cleanSongName));

    cleanupYoutubeSearch();

    if (!loadCachedYoutubeResults())
        startYoutubeSearch();
}

#define YOUTUBE_RESULTS 10
// TODO there may not be 10 results ... update maxpos at youtube-dl eof


bool SoundFix::loadCachedYoutubeResults()
{
    int i=0;
    for (; i<YOUTUBE_RESULTS; i++) {
        QFile resultFile(QString("data/%1 - result %2.txt").arg(cleanSongName).arg(i+1));
        if (!resultFile.open(QFile::ReadOnly))
            break;

        QTextStream resultStream(&resultFile);
        resultStream.setCodec("UTF-8");

        QString title     = resultStream.readLine();
        QString url       = resultStream.readLine();
        QString thumbnail = resultStream.readLine();

        youtubeLines[0] = title;
        youtubeLines[1] = url;
        youtubeLines[2] = thumbnail;

        thumbsFinished = i;
        youtubeAddResult();
        showThumb();
    }

    return i>0;
}

void SoundFix::startYoutubeSearch()
{    
    printf("\nyoutube search\n");

    identProgressBar.setLabelText("Starting YouTube video search...");
    identProgressBar.setMinimum(0);
    identProgressBar.setMaximum(2*YOUTUBE_RESULTS);
    identProgressBar.setValue(0);
    identProgressBar.setMinimumDuration(100);

    ui->youtubeTable->setRowCount(0);

    connect(&youtubeSearchProc, SIGNAL(readyReadStandardOutput()), this, SLOT(youtubeReadyRead()));
    connect(&youtubeSearchProc, SIGNAL(finished(int)), this, SLOT(youtubeFinished(int)));
    connect(&youtubeSearchProc, SIGNAL(error(QProcess::ProcessError)),
            this, SLOT(youtubeError(QProcess::ProcessError)));

    connect(thumbMgr, SIGNAL(finished(QNetworkReply*)),
            this, SLOT(thumbnailFinished(QNetworkReply*)));

    youtubeSearchProc.setWorkingDirectory("data");

    // nondebug
    youtubeSearchProc.start("tools/youtube-dl.exe", QStringList() <<
            QString("ytsearch10:%1").arg(cleanSongName) <<
            "--cookies" << "cookies.txt" <<
            "-g" << "-e" << "--get-thumbnail");

    // debug
    //youtubeSearchProc.start("python.exe ../tools/delay.py ../tools/search.txt 0");

    ui->youtubeTable->setFocus();
}

void SoundFix::cleanupYoutubeSearch()
{
    printf("youtube search cleanup\n");

    disconnect(&youtubeSearchProc, SIGNAL(readyReadStandardOutput()), this, SLOT(youtubeReadyRead()));
    disconnect(&youtubeSearchProc, SIGNAL(finished(int)), this, SLOT(youtubeFinished(int)));
    disconnect(&youtubeSearchProc, SIGNAL(error(QProcess::ProcessError)),
            this, SLOT(youtubeError(QProcess::ProcessError)));

    disconnect(thumbMgr, SIGNAL(finished(QNetworkReply*)),
            this, SLOT(thumbnailFinished(QNetworkReply*)));

    youtubeSearchProc.kill();

    thumbUrls.clear();

    youtubeLineNo = 0;
    thumbsStarted = 0;
    thumbsFinished = 0;

    identProgressBar.setValue(identProgressBar.maximum());
}

void SoundFix::youtubeUpdateProgress()
{
    identProgressBar.setValue(youtubeLineNo/3 + thumbsFinished);
}

// thumbUrl = "..../CU8V4BSuRKI/default.jpg"

QString getVideoId(const QString &thumbUrl)
{
    int rpos = thumbUrl.lastIndexOf('/');
    if (rpos<0) return QString();

    int lpos = thumbUrl.lastIndexOf('/', rpos-1);
    if (lpos<0) return QString();

    return thumbUrl.mid(lpos+1, rpos-1-lpos);
}

void SoundFix::youtubeAddResult()
{    
    int row = ui->youtubeTable->rowCount();
    ui->youtubeTable->insertRow(row);

    ui->youtubeTable->verticalHeader()->resizeSection(row, 60);

    if (row==0) ui->youtubeTable->selectRow(0);

    // col 0 ... just store the thumbUrl here, it's hidden

    ui->youtubeTable->setItem(row, 0, new QTableWidgetItem(youtubeLines[2]));

    // col 3

    QString videoId = getVideoId(youtubeLines[2]);

    QString link = videoId.isEmpty() ? youtubeLines[0] :
             QString("<a href=\"http://www.youtube.com/watch?v=%1\">%2</a>")
                    .arg(videoId).arg(Qt::escape(youtubeLines[0]));

    QLabel *href = new QLabel(link, this);
    href->setOpenExternalLinks(true);

    QFont font = href->font();
    font.setPointSize(11);
    font.setBold(true);
    href->setFont(font);

    ui->youtubeTable->setCellWidget(row, 3, href);
}

void SoundFix::showThumb()
{
    // col 2

    QWidget* w2 = new QWidget;
    QLabel *label = new QLabel(w2);

    QString thumbFname = QString("data/%1 - result %2.jpg").arg(cleanSongName).arg(thumbsFinished);
    QPixmap pixmap(thumbFname);
    QPixmap scaledPix = pixmap.scaled(90, 60);
    label->setPixmap(scaledPix);

    QHBoxLayout* layout2 = new QHBoxLayout(w2);
    layout2->addWidget(label);
    layout2->setAlignment(Qt::AlignCenter);
    layout2->setSpacing(0);
    layout2->setMargin(0);

    w2->setLayout(layout2);

    ui->youtubeTable->setCellWidget(thumbsFinished, 2, w2);
}

void SoundFix::youtubeReadyRead()
{
    while (youtubeSearchProc.canReadLine()) {
        QString line = youtubeSearchProc.readLine().trimmed();
        if (line.isEmpty()) continue;

        youtubeLines[youtubeLineNo++ % 3] = line;

        if (youtubeLineNo % 3 == 0) {
            youtubeUpdateProgress();

            printf("title: [%s]\n",     youtubeLines[0].toAscii().data());
            printf("url: [%s]\n",       youtubeLines[1].toAscii().data());
            printf("thumbnail: [%s]\n", youtubeLines[2].toAscii().data());
            printf("\n");

            QFile resultFile(QString("data/%1 - result %2.txt").arg(cleanSongName).arg(youtubeLineNo/3 - 1));
            if (resultFile.open(QFile::WriteOnly)) {
                QTextStream resultStream(&resultFile);
                resultStream.setCodec("UTF-8");
                resultStream << youtubeLines[0] << "\n";
                resultStream << youtubeLines[1] << "\n";
                resultStream << youtubeLines[2] << "\n";
            }

            youtubeAddResult();

            thumbUrls.append(youtubeLines[2]);
            if (thumbsFinished == thumbsStarted)
                startThumbnail();
        }
    }
}

void SoundFix::startThumbnail()
{
    printf("starting thumbnail %d\n", thumbsStarted);

    thumbMgr->get(QNetworkRequest(QUrl(thumbUrls[thumbsStarted])));

    thumbsStarted++;
}

bool SoundFix::saveThumb(const QByteArray &data)
{
    QFile f(QString("data/%1 - result %2.jpg").arg(cleanSongName).arg(thumbsFinished));
    if (!f.open(QFile::WriteOnly))
        return false;

    f.write(data);
    f.close();
    return true;
}

void SoundFix::thumbnailFinished(QNetworkReply *reply)
{
    printf("thumbnail finished %d\n", thumbsFinished);

    if (saveThumb(reply->readAll()))
        showThumb();

    reply->deleteLater();

    thumbsFinished++;
    youtubeUpdateProgress();

    if (thumbUrls.length() > thumbsFinished)
        startThumbnail();

    if (!youtubeSearchProc.isOpen())
        cleanupYoutubeSearch();
}

void SoundFix::youtubeError(QProcess::ProcessError)
{
    error("YouTube search error", "Could not start YouTube search.");
    cleanupYoutubeSearch();
}

void SoundFix::youtubeFinished(int exitCode)
{
    printf("youtube search finished\n");

    if (exitCode != 0) {
        error("YouTube search error", "YouTube search returned an error.");
        cleanupYoutubeSearch();
        return;
    }

    // TODO update progress, etc with actual number of results

    if (thumbsFinished == thumbUrls.length())
        cleanupYoutubeSearch();
}

void SoundFix::on_downloadBtn_clicked()
{
    QList<QModelIndex> rows = ui->youtubeTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        information("No video is selected", "Please select a video to download from the list.");
        return;
    }
    int row = rows.first().row();

    QString thumbUrl = ui->youtubeTable->item(row, 0)->text();
    if (thumbUrl.isEmpty())
        return;

    QString videoId = getVideoId(thumbUrl);
    if (videoId.isEmpty())
        return;

    cleanupYoutubeDown();

    if (findCachedYoutubeVideo(videoId))
        runAudioSync();
    else
        startYoutubeDown(videoId);
}

bool SoundFix::findCachedYoutubeVideo(const QString &videoId)
{
    youtubeDownDestination.clear();

    QString fnameMp4 = videoId + ".mp4";
    QString fnameFlv = videoId + ".flv";

    if (QFile(QString("data/")+ fnameMp4).exists())
        youtubeDownDestination = fnameMp4;
    else if (QFile(QString("data/")+ fnameFlv).exists())
        youtubeDownDestination = fnameFlv;
    else return false;

    ui->downloadProgress->setValue(1000);
    ui->progressLabel->setText("Already downloaded.");

    return true;
}

void SoundFix::startYoutubeDown(const QString &videoId)
{
    printf("\nyoutube down\n");

    connect(&youtubeDownProc, SIGNAL(readyReadStandardOutput()), this, SLOT(youtubeDownReadyRead()));
    connect(&youtubeDownProc, SIGNAL(finished(int)), this, SLOT(youtubeDownFinished(int)));
    connect(&youtubeDownProc, SIGNAL(error(QProcess::ProcessError)),
            this, SLOT(youtubeDownError(QProcess::ProcessError)));

    youtubeDownStdout.clear();
    youtubeDownDestination.clear();

    videoId.isEmpty();

    youtubeDownProc.setWorkingDirectory("data");

    // nondebug
    youtubeDownProc.start("tools/youtube-dl.exe", QStringList() <<
            QString("http://www.youtube.com/watch?v=%1").arg(videoId) <<
            "--max-quality" << "18" <<
            "--cookies" << "cookies.txt");

    // debug
    //youtubeDownProc.start("python.exe ../tools/delay.py ../tools/download.txt 0.00");
}

void SoundFix::cleanupYoutubeDown()
{
    printf("youtube down cleanup\n");

    disconnect(&youtubeDownProc, SIGNAL(readyReadStandardOutput()), this, SLOT(youtubeDownReadyRead()));
    disconnect(&youtubeDownProc, SIGNAL(finished(int)), this, SLOT(youtubeDownFinished(int)));
    disconnect(&youtubeDownProc, SIGNAL(error(QProcess::ProcessError)),
            this, SLOT(youtubeDownError(QProcess::ProcessError)));

    youtubeDownProc.kill();

    ui->downloadProgress->setValue(0);

    ui->offsetsTable->setRowCount(0);
}

void SoundFix::youtubeDownReadyRead()
{
    youtubeDownStdout.append(youtubeDownProc.readAll());

    for (;;) {
        // it separates progress lines with \r and other lines with \n
        int cr = youtubeDownStdout.indexOf('\r');
        int lf = youtubeDownStdout.indexOf('\n');
        if (cr<0 && lf<0) break;

        int sep = (cr<0 || (lf>0 && lf<cr)) ? lf : cr;
        int nonsep = sep;

        QString sepChars("\r\n");
        while (nonsep < youtubeDownStdout.length() && sepChars.contains(youtubeDownStdout.at(nonsep)))
            nonsep++;

        QString line = youtubeDownStdout.left(sep);
        youtubeDownStdout = youtubeDownStdout.right(youtubeDownStdout.length() - nonsep);

        // get destination file

        QRegExp rxd("\\[download\\] +Destination: +(.+)");
        if (rxd.indexIn(line)>=0) {
            youtubeDownDestination = rxd.cap(1);
            printf("destination: %s\n", youtubeDownDestination.toAscii().data());
        }

        QRegExp rxd2("\\[download\\] +([^ ]+) +has already been downloaded");
        if (rxd2.indexIn(line)>=0) {
            youtubeDownDestination = rxd2.cap(1);
            printf("destination-already: %s\n", youtubeDownDestination.toAscii().data());
        }

        // get progress

        QRegExp rxp("\\[download\\] +(.+)%.* at (.+)/s +ETA +([^ ]+)");
        if (rxp.indexIn(line)>=0) {
            QString percent = rxp.cap(1);
            QString speed = rxp.cap(2);
            QString eta = rxp.cap(3);

            printf("percent=%s speed=%s eta=%s\n",
                   percent.toAscii().data(),
                   speed.toAscii().data(),
                   eta.toAscii().data());

            float fpercent = percent.toFloat()*10;
            ui->downloadProgress->setValue(fpercent);

            ui->progressLabel->setText(QString("%1/s, ETA %2").arg(speed).arg(eta));
        }
    }
}

void SoundFix::youtubeDownError(QProcess::ProcessError)
{
    error("YouTube download error", "Could not download YouTube video.");
    cleanupYoutubeSearch();
}

void SoundFix::youtubeDownFinished(int exitCode)
{
    printf("youtube down finished\n");

    if (exitCode != 0) {
        error("YouTube download error", "YouTube download was interrupted.");
        cleanupYoutubeSearch();
        return;
    }

    ui->progressLabel->setText("Download complete.");

    if (youtubeDownDestination.isEmpty()) {
        error("YouTube download error", "Could not determine YouTube download location.");
        cleanupYoutubeSearch();
        return;
    }

    runAudioSync();
}

void SoundFix::cleanupAudioSync()
{
    syncProgressBar.setValue(1400);

    ui->tempoCombo->clear();
    ui->tempoCombo->addItem("100%");
}

int progressCallback(void *arg, const char *step, int progress)
{
    SoundFix *ptr = (SoundFix *)arg;
    return ptr->updateAudioSyncProgress(step, progress);
}

int SoundFix::updateAudioSyncProgress(const char *step, int progress)
{
    if (step) syncProgressBar.setLabelText(step);
    syncProgressBar.setValue(300+progress);

    QApplication::processEvents();

    return syncProgressBar.wasCanceled();
}

void SoundFix::runAudioSync()
{
    cleanupAudioSync();

    syncProgressBar.setCancelButtonText("Cancel");
    syncProgressBar.setMinimum(0);
    syncProgressBar.setMaximum(1400);
    syncProgressBar.setMinimumDuration(100);

    syncProgressBar.setLabelText("Extracting audio...");
    syncProgressBar.setValue(0);

    QString recordingSampleFname = QString("data/%1 - sample.wav").arg(recordingName);

    QString youtubeSampleFname = QString("data/%1 - sample.wav").arg(youtubeDownDestination);
    QString youtubeTempoFname = QString("data/%1 - tempo.wav").arg(youtubeDownDestination);
    QFile(youtubeSampleFname).remove();
    QFile(youtubeTempoFname).remove();

    QProcess ffmpegYtWav;
    ffmpegYtWav.start("tools/ffmpeg.exe", QStringList() << "-i" <<
            QString("data/%1").arg(youtubeDownDestination) <<
            "-f" << "wav" <<
            "-ac" << "1" <<
            "-ar" << "44100" <<
            youtubeSampleFname);
    if (!ffmpegYtWav.waitForFinished() || ffmpegYtWav.exitCode() != 0) {
        error("Audio load error", "Cannot extract audio from youtube.");
        cleanupAudioSync();
        return;
    }

    // specpp_compare calls our callback with labels and
    // progress values [0...1000] which we need to offset by whatever (see setMaximum)

    // nondebug
    if (!specpp_compare(youtubeSampleFname.utf16(), recordingSampleFname.utf16(),
            progressCallback, this,
            //scores
            3, MAX_SYNC_OFFSETS, 75, &retOffsets, offsets, confidences, &tempoRatio))
    {
        error("Audio sync error", "Cannot synchronize audio tracks.");
        cleanupAudioSync();
        return;
    }

    // tempo-scale the youtube wav for mixing

    syncProgressBar.setLabelText("Adjusting YouTube tempo to recording tempo...");
    syncProgressBar.setValue(1350);
    QApplication::processEvents();

    QProcess ffmpegScaleYt;
    ffmpegScaleYt.start("tools/ffmpeg.exe", QStringList() <<
            "-i" << youtubeSampleFname <<
            "-af" << QString().sprintf("atempo=%.3f", 1.0/tempoRatio) <<
            youtubeTempoFname);
    if (!ffmpegScaleYt.waitForFinished() || ffmpegScaleYt.exitCode() != 0) {
        error("Audio load error", "Cannot adjust tempo of YouTube audio.");
        cleanupAudioSync();
        return;
    }

    syncProgressBar.setValue(1400);

    if (tempoRatio == 1.0) {
        ui->tempoInfoLabel->setText("(not required)");
    } else {
        // there is already '100%' in there
        ui->tempoCombo->addItem(QString().sprintf("%.1f%%", tempoRatio*100.0));
        ui->tempoInfoLabel->setText("");
    }

    // get a 'confident' bool from specpp_compare and only select the bpm if it's true

    ui->tempoCombo->setCurrentIndex(ui->tempoCombo->count()-1);

    // debug
    /*syncProgressBar.setValue(1400);

    retOffsets = 2;
    offsets[0] = 403000;
    confidences[0] = 100.0f;
    offsets[1] = 503000;
    confidences[1] = 95.5f;*/

    // add offsets

    for (int i=0; i<retOffsets; i++) {
        int row = ui->offsetsTable->rowCount();
        ui->offsetsTable->insertRow(row);
        if (row==0) ui->offsetsTable->selectRow(0);

        QWidget* w0 = new QWidget;
        QPushButton *button = new QPushButton(w0);
        button->setIcon(QIcon("play.png"));
        QHBoxLayout* layout0 = new QHBoxLayout(w0);
        layout0->addWidget(button);
        layout0->setAlignment(Qt::AlignCenter);
        layout0->setSpacing(0);
        layout0->setMargin(0);
        w0->setLayout(layout0);
        ui->offsetsTable->setCellWidget(row, 0, w0);

        connect(button, SIGNAL(clicked()), this, SLOT(playOffset()));

        QString offstr = QString().sprintf("%.2f sec", ((float)offsets[i])/44100.0f);
        ui->offsetsTable->setItem(row, 1, new QTableWidgetItem(offstr));

        QString confstr = QString().sprintf("%.1f%%", confidences[i]);
        ui->offsetsTable->setItem(row, 2, new QTableWidgetItem(confstr));
    }
}

QPushButton *SoundFix::buttonFromOffsetRow(int row)
{
    QHBoxLayout *layout = (QHBoxLayout *)ui->offsetsTable->cellWidget(row, 0);
    return (QPushButton *)layout->children().first();
}

void SoundFix::playSyncAudio(int row)
{
    if (!player->isValid()) {
        printf("sound not available\n");
        QProcess *startWav = new QProcess(this);
        connect(startWav, SIGNAL(finished(int)), startWav, SLOT(deleteLater()));
        connect(startWav, SIGNAL(error(QProcess::ProcessError)), startWav, SLOT(deleteLater()));

        startWav->start("cmd.exe /c start ", QStringList() << "data/mix.wav");
    } else {
        buttonFromOffsetRow(row)->setIcon(QIcon("stop.png"));
        player->setCurrentSource(QUrl("data/mix.wav"));
        player->play();
        playingRow = row;
    }
}

void SoundFix::playerStateChanged(Phonon::State state)
{
    if (state == Phonon::PausedState) {
        QPushButton *playingButton = buttonFromOffsetRow(playingRow);
        if (!playingButton) return;

        playingButton->setIcon(QIcon("play.png"));
        playingRow = -1;
    }
}

void SoundFix::playOffset()
{
    QPushButton *button = (QPushButton *)QObject::sender();
    if (!button) return;

    player->stop();
    player->setCurrentSource(QUrl(""));

    if (playingRow >= 0) {
        QPushButton *playingButton = buttonFromOffsetRow(playingRow);
        if (!playingButton) return;

        playingButton->setIcon(QIcon("play.png"));

        playingRow = -1;

        if (playingButton == button)
            return;
    }

    int row;
    for (row=0; row<ui->offsetsTable->rowCount(); row++)
        if (buttonFromOffsetRow(row) == button)
            break;

    if (row==ui->offsetsTable->rowCount())
        return;

    ui->offsetsTable->selectRow(row);

    QString youtubeTempoFname = QString("data/%1 - tempo.wav").arg(youtubeDownDestination);

    if (!specpp_mix((int)(offsets[row] * tempoRatio), youtubeTempoFname.utf16(), "data/mix.wav"))
        { error("Error mixing audio tracks", "Could not create audio mix."); return; }

    playSyncAudio(row);
}

void SoundFix::on_saveBtn_clicked()
{
    QList<QModelIndex> rows = ui->offsetsTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        information("No offset is selected", "Please select a time offset from the list above.");
        return;
    }
    int row = rows.first().row();

    QString outputName = QFileDialog::getSaveFileName(this, "Save Video", QString(), "MP4 Videos (*.mp4)");
    if (outputName.isNull())
        return;

    QFile(outputName).remove();

    QString ss = QString().sprintf("%.2f", ((float)offsets[row])/44100.0f);
    QString t = QString().sprintf("%.2f", ((float)durationMsec)/1000.0f);

    QProcess ffmpegMerge;

    QString youtubeSampleFname = QString("data/%1 - sample.wav").arg(youtubeDownDestination);

    QStringList args;
    args <<
        "-ss" << ss <<
        "-i" << youtubeSampleFname <<
        //"-ss" << "0" << // winff: needed; zeranoe-ffmpeg - not
        //"-t" << t << // ditto
        "-i" << recordingPath <<
        "-map" << "0:0" <<
        "-map" << "1:video" <<
        "-f" << "mp4" <<
        "-vcodec" << "copy" <<
        "-acodec" << "libvo_aacenc" << // winff: "libfaac"
        "-t" << t; // winff: this was after -ss

    if (tempoRatio != 1.0)
        args << "-af" << QString().sprintf("atempo=%.3f", 1.0/tempoRatio);

    args << outputName;

    printf("running ffmpeg %s\n", args.join(" ").toAscii().data());

    QProgressDialog encodeProgress;
    encodeProgress.setLabelText("Encoding video...");
    encodeProgress.setMinimum(0);
    encodeProgress.setMaximum(100);
    encodeProgress.setMinimumDuration(100);
    encodeProgress.setValue(0);
    encodeProgress.show();

    QApplication::processEvents();

    ffmpegMerge.start("tools/ffmpeg.exe", args);
    if (!ffmpegMerge.waitForFinished() || ffmpegMerge.exitCode() != 0)
        { error("Video encoder error", "Error while re-encoding video with audio from YouTube."); return; }

    encodeProgress.setValue(100);
    encodeProgress.hide();
}
