/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#ifndef PROGRESSDISPATCHER_H
#define PROGRESSDISPATCHER_H

#include "owncloudlib.h"
#include <QObject>
#include <QHash>
#include <QTime>
#include <QQueue>
#include <QElapsedTimer>
#include <QDebug>

#include "syncfileitem.h"

namespace OCC {

/**
 * @brief The FolderScheduler class schedules folders for sync
 */
namespace Progress
{
    /** Return true is the size need to be taken in account in the total amount of time */
    inline bool isSizeDependent(csync_instructions_e instruction) {
        return instruction == CSYNC_INSTRUCTION_CONFLICT || instruction == CSYNC_INSTRUCTION_SYNC
            || instruction == CSYNC_INSTRUCTION_NEW;
    }


    struct Info {
        Info() : _totalFileCount(0), _totalSize(0), _completedFileCount(0), _completedSize(0) {}

        // Used during local and remote update phase
        QString _currentDiscoveredFolder;

        quint64 _totalFileCount;
        quint64 _totalSize;
        quint64 _completedFileCount;
        quint64 _completedSize;
        // Should this be in a separate file?
        struct EtaEstimate {
            EtaEstimate() :  _startedTime(QDateTime::currentMSecsSinceEpoch()), _agvEtaMSecs(0),_effectivProgressPerSec(0),_divergince(0),_sampleCount(1) {}
            
            static const int MAX_AVG_DIVIDER=2048;
            static const int MIN_SAMPLE_COUNT=10;
            static const int INITAL_WAIT_TIME=0;
            static constexpr float MAX_STATS_DIVERGANCE_RATIO=0.0833f;
            static constexpr float PREFFERED_STATS_DIVERGANCE_RATIO=0.05f;
            static constexpr float GOOD_DIVERGANCE_RATIO=0.005f;
            
            quint64     _startedTime ;
            quint64     _agvEtaMSecs;
            quint64     _effectivProgressPerSec;
            float       _divergince;
            float      _sampleCount;
            
            /**
             * reset the estiamte.
             */
            void reset() {
                _startedTime = QDateTime::currentMSecsSinceEpoch();
                _divergince = 0;
                _sampleCount =1;
                _effectivProgressPerSec = _agvEtaMSecs = 0;
            }
            
            /**
             * update the estimated eta time with more current data.
             * @param quint64 completed the amount the was completed.
             * @param quint64 total the total amout that should be completed.
             */
            void updateTime(quint64 completed, quint64 total) {
                quint64 elapsedTime = QDateTime::currentMSecsSinceEpoch() -  this->_startedTime;
                //don't start until you have some good data to process, prevents jittring estiamtes at the start of the syncing process
                if(completed == 0) {
                    this->reset();
                    return;
                }
                if(total != 0 && elapsedTime > INITAL_WAIT_TIME ) {
                     // (elapsedTime-1) is an hack to avoid float "un-rounding" issue (ie. 0.99999999999999999999....)
                    quint64 currentEstimate = (((static_cast<float>(total) / completed) * elapsedTime) - (elapsedTime-1));
                    
                    this->updateSampleCount();
                   
                    _agvEtaMSecs += currentEstimate - this->getEtaEstimate();
                    _effectivProgressPerSec = ( total - completed ) / (1+this->getEtaEstimate()/1000);
                    _divergince += (static_cast<float>(currentEstimate+this->_startedTime) / (this->getEtaEstimate()+this->_startedTime)) - ( _divergince / _sampleCount );
                   
                    
                }
            }
            
            /**
             * Increase or  decresce the eta sampling based on consistency of it.
             */
            void updateSampleCount() {
                float diff = 0; 
                if( _sampleCount < MAX_AVG_DIVIDER && (this->getSampleDivergance() > PREFFERED_STATS_DIVERGANCE_RATIO || !this->isEstimationReady() ) ) {
                    diff = 1;
                    
                } else if (this->getSampleDivergance() < GOOD_DIVERGANCE_RATIO && _sampleCount > MIN_SAMPLE_COUNT ) {
                    diff = -1;  
                }
                if(diff) {
                    _sampleCount += diff;
                    _agvEtaMSecs = _agvEtaMSecs * (1+ (diff/_sampleCount));
                    _divergince =_divergince * (1+ (diff/_sampleCount));
                }
            }
            
            /**
             * Get the eta estimate in milliseconds 
             * @return quint64 the estimate amount of milliseconds to end the process.
             */
            quint64 getEtaEstimate() const {
               return  _agvEtaMSecs / _sampleCount;
           }
            
           /**
            * Get the estimated average bandwidth usage.
            * @return quint64 the estimated bandwidth usage in bytes.
            */
           quint64 getEstimatedBandwidth() const {
               return _effectivProgressPerSec;
           }
           /**
            * Check if the estimation is ready and will return good information.
            */ 
           float getSampleDivergance() const {
               return  sqrt( (1- (_divergince / _sampleCount)) * (1- (_divergince / _sampleCount)) ) ;
           }
           
           /**
            * Check if the estimation is ready and will return good information.
            */ 
           bool isEstimationReady() const{
               return _sampleCount > MIN_SAMPLE_COUNT && this->getSampleDivergance() < MAX_STATS_DIVERGANCE_RATIO;
           }
        };
        EtaEstimate _totalEtaEstimate;

        struct ProgressItem {
            ProgressItem() : _completedSize(0) {}
            SyncFileItem _item;
            quint64 _completedSize;
            EtaEstimate _etaEstimate;
        };
        QHash<QString, ProgressItem> _currentItems;
        SyncFileItem _lastCompletedItem;

        void setProgressComplete(const SyncFileItem &item) {
            _currentItems.remove(item._file);
            _completedFileCount += item._affectedItems;
            if (!item._isDirectory) {
                if (Progress::isSizeDependent(item._instruction)) {
                    _completedSize += item._size;
                }
            } 
            _lastCompletedItem = item;
            this->updateEstimation();
        }
        void setProgressItem(const SyncFileItem &item, quint64 size) {
            _currentItems[item._file]._item = item;
            _currentItems[item._file]._completedSize = size;
            _lastCompletedItem = SyncFileItem();
            this->updateEstimation();
            _currentItems[item._file]._etaEstimate.updateTime(size,item._size);
        }
        
        void updateEstimation() {
            if(this->_totalSize > 0) {
                _totalEtaEstimate.updateTime(this->completedSize(),this->_totalSize);
            } else {
                _totalEtaEstimate.updateTime(this->_completedFileCount,this->_totalFileCount);
            }
        }

        quint64 completedSize() const {
            quint64 r = _completedSize;
            foreach(const ProgressItem &i, _currentItems) {
                if (!i._item._isDirectory)
                    r += i._completedSize;
            }
            return r;
        }
        /**
         * Get the total completion estimate structure 
         * @return EtaEstimate a structure containing the total completion information.
         */
        EtaEstimate totalEstimate() const {
            return _totalEtaEstimate;
        }

        /**
         * Get the current file completion estimate structure 
         * @return EtaEstimate a structure containing the current file completion information.
         */
        EtaEstimate getFileEstimate(const SyncFileItem &item) const {
            return _currentItems[item._file]._etaEstimate;
        }
        
    };

    OWNCLOUDSYNC_EXPORT QString asActionString( const SyncFileItem& item );
    OWNCLOUDSYNC_EXPORT QString asResultString(  const SyncFileItem& item );

    OWNCLOUDSYNC_EXPORT bool isWarningKind( SyncFileItem::Status );

    /**
     * @brief Get the estimated finish time as printable string from a progess EtaEstimate object.
     * @param EtaEstimate estimate the finish estimate object.
     * @param uint precision the amount of sub dviving scale to include in the result.
     * @return an HMS representation of the milliseconds value.
     */
    OWNCLOUDSYNC_EXPORT QString estimateToString(const Progress::Info::EtaEstimate& estimate, quint8 precision);
}

/**
 * @file progressdispatcher.h
 * @brief A singleton class to provide sync progress information to other gui classes.
 *
 * How to use the ProgressDispatcher:
 * Just connect to the two signals either to progress for every individual file
 * or the overall sync progress.
 *
 */
class OWNCLOUDSYNC_EXPORT ProgressDispatcher : public QObject
{
    Q_OBJECT

    friend class Folder; // only allow Folder class to access the setting slots.
public:
    static ProgressDispatcher* instance();
    ~ProgressDispatcher();

signals:
    /**
      @brief Signals the progress of data transmission.

      @param[out]  folder The folder which is being processed
      @param[out]  progress   A struct with all progress info.

     */
    void progressInfo( const QString& folder, const Progress::Info& progress );
    /**
     * @brief: the item's job is completed
     */
    void jobCompleted(const QString &folder, const SyncFileItem & item);

    void syncItemDiscovered(const QString &folder, const SyncFileItem & item);

protected:
    void setProgressInfo(const QString& folder, const Progress::Info& progress);

private:
    ProgressDispatcher(QObject* parent = 0);

    QElapsedTimer _timer;
    static ProgressDispatcher* _instance;
};

}
#endif // PROGRESSDISPATCHER_H
