#pragma once

namespace Ui
{
    struct PlayingData
    {
        PlayingData()
            : Source_(0)
            , Buffer_(0)
            , Id_(-1)
        {
        }

        void init();
        void setBuffer(const QByteArray& data, qint64 freq, qint64 fmt);
        void play();
        void pause();
        void stop();
        void clear();
        void free();
        bool isEmpty() const;
        openal::ALenum state() const;

        openal::ALuint Source_;
        openal::ALuint Buffer_;
        int Id_;
    };

	class SoundsManager : public QObject
	{
		Q_OBJECT
Q_SIGNALS:
        void pttPaused(int);
        void pttFinished(int, bool);

	public:
		SoundsManager();
		~SoundsManager();

		void playIncomingMessage();
		void playOutgoingMessage();

        int playPtt(const QString& file, int id);
        void pausePtt(int id);

		void callInProgress(bool value);

        void reinit();

	private Q_SLOTS:
		void timedOut();
        void checkPttState();
        void contactChanged(QString);

        void initOpenAl();
        void shutdownOpenAl();
        void initIncomig();
        void initOutgoing();

	private:
		bool CallInProgress_;
		bool CanPlayIncoming_;

        PlayingData Incoming_;
        PlayingData Outgoing_;
		QTimer* Timer_;
        
        QTimer* PttTimer_;
        int AlId;
        openal::ALCdevice *AlAudioDevice_;
        openal::ALCcontext *AlAudioContext_;
        PlayingData CurPlay_;
        PlayingData PrevPlay_;
        bool AlInited_;
	};

	SoundsManager* GetSoundsManager();
}
