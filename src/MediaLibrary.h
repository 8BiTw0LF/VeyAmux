// VeyAmux for Linux: the Media page. Films and series from the user's own folders, Jellyfin servers and Plex servers,
// shown the same way (posters, folders, "continue watching"), played through VeyAmux's normal file path with every
// enhancement. Server streams are played as they are stored (no transcoding on the server): FFmpeg decodes them here.
//
//   Jellyfin: signed in with a user name and password, or with Quick Connect (a code confirmed in another Jellyfin app)
//   Plex:     signed in on plex.tv in the browser (a PIN); every server of the account is added. Outside the home
//             network Plex wants Plex Pass or a Remote Watch Pass; at home it is free.
//
// Tokens are kept in the settings under keys containing "token" (the debug report hides those), and never put into
// a URL: they go in HTTP headers, also to FFmpeg.
#pragma once
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class PlayerBridge;
class QNetworkReply;

namespace veyamux {

class MediaLibrary : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)   // [{id, name, kind}]
    Q_PROPERTY(QString current READ current NOTIFY viewChanged)
    Q_PROPERTY(QVariantList crumbs READ crumbs NOTIFY viewChanged)        // titles below the source
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    // the posters as they arrive: image(key) after a change of imageVersion (one lookup per card; a map handed to QML
    // was copied whole for every card on every change, which froze the page with large libraries)
    Q_PROPERTY(int imageVersion READ imageVersion NOTIFY imagesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // signing in: {kind: "quick" | "plex", code, url} while a code waits to be confirmed, else empty
    Q_PROPERTY(QVariantMap login READ login NOTIFY loginChanged)
    Q_PROPERTY(bool playingFromLibrary READ playingFromLibrary NOTIFY playbackChanged)
public:
    explicit MediaLibrary(PlayerBridge* player, QObject* parent = nullptr);
    ~MediaLibrary() override;

    QVariantList sources() const;
    QString current() const { return current_; }
    QVariantList crumbs() const;
    QVariantList items() const { return items_; }
    int imageVersion() const { return imageVersion_; }
    Q_INVOKABLE QString image(const QString& key) const { return images_.value(key); }
    // a card came on screen: its poster is fetched next (the ones out of sight wait until they are scrolled to)
    Q_INVOKABLE void want(const QString& key);
    bool busy() const { return pending_ > 0; }
    QVariantMap login() const { return login_; }
    bool playingFromLibrary() const { return !playing_.source.isEmpty(); }

    Q_INVOKABLE void select(const QString& sourceId);
    Q_INVOKABLE void openItem(int index);                // a folder: into it
    Q_INVOKABLE bool back();                             // one level up; false at the top
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void play(int index, bool fromStart);
    Q_INVOKABLE void addFolder();
    Q_INVOKABLE void removeSource(const QString& id);
    Q_INVOKABLE void addJellyfin(const QString& url, const QString& user, const QString& password);
    Q_INVOKABLE void jellyfinQuickConnect(const QString& url);
    Q_INVOKABLE void addPlex();
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void stopPlayback();                     // reports the position and closes the video

signals:
    void sourcesChanged();
    void viewChanged();
    void itemsChanged();
    void imagesChanged();
    void busyChanged();
    void loginChanged();
    void playbackChanged();
    void message(const QString& key, const QString& arg, bool error);   // a Strings.js key and its %1

private:
    struct Level { QString title, kind, id, extra; };   // kind: what the id is (root, folder, jf-items, plex-key, ...)
    struct Source { QString id, kind, name, url, user, token, machine; QStringList uris; };
    struct Playing { QString source, item, kind; double duration = 0; double lastPos = -1; bool started = false; };

    // sources
    QList<Source> loadSources() const;
    void saveSource(const Source& s);
    Source source(const QString& id) const;
    QString deviceId() const;
    // listing
    void list();
    void listLocal(const Level& l);
    void listJellyfin(const Source& s, const Level& l);
    void listPlex(const Source& s, const Level& l);
    void setItems(QVariantList items, int gen);
    static QVariantList categoryFolders(const QString& itemsLevel, const QString& genresLevel, const QString& id, const QString& extra);
    // network
    QNetworkReply* get(const Source& s, const QString& path, std::function<void(int status, const QByteArray& body)> done, int timeoutMs = 20000);
    QNetworkReply* post(const Source& s, const QString& path, const QByteArray& json, std::function<void(int status, const QByteArray& body)> done);
    void request(QNetworkReply* r, std::function<void(int, const QByteArray&)> done, int timeoutMs);
    QString jellyfinAuth(const QString& token) const;
    void plexHeaders(class QNetworkRequest& rq, const QString& token) const;
    void plexConnect(const Source& s, std::function<void(bool)> done);
    void loadImage(int gen, const QString& key, const Source& s, const QString& path, const QString& cacheName);
    void pumpImages();
    // playback
    void startPlayback(const Source& s, const QVariantMap& item, bool fromStart);
    void fetchSubtitles(const Source& s, const QString& item);
    void report(const QString& state);
    void jellyfinSignedIn(const QString& url, const QString& name, const QByteArray& authBody);

    PlayerBridge* player_;
    QNetworkAccessManager net_;
    QString current_;
    QList<Level> levels_;
    QVariantList items_;
    QHash<QString, QString> images_;
    int imageVersion_ = 0;
    int gen_ = 0;                    // listing generation: late answers for an old listing are dropped
    int pending_ = 0;
    QVariantMap login_;
    QTimer loginPoll_;
    QHash<QString, QString> plexUri_;   // server -> the connection that answered
    struct ImageJob { int gen; QString key; Source src; QString path, file; };
    QList<ImageJob> imageQueue_;                 // wanted (on screen), the last wanted first
    QHash<QString, ImageJob> imageWaiting_;      // not on screen yet
    int imagesRunning_ = 0;
    QTimer imagesChanged_;
    Playing playing_;
    QTimer progress_;
};

} // namespace veyamux
