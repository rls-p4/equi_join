/*
**
* BEGIN_COPYRIGHT
*
* Copyright (C) 2008-2016 SciDB, Inc.
* All Rights Reserved.
*
* equi_join is a plugin for SciDB, an Open Source Array DBMS maintained
* by Paradigm4. See http://www.paradigm4.com/
*
* equi_join is free software: you can redistribute it and/or modify
* it under the terms of the AFFERO GNU General Public License as published by
* the Free Software Foundation.
*
* equi_join is distributed "AS-IS" AND WITHOUT ANY WARRANTY OF ANY KIND,
* INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,
* NON-INFRINGEMENT, OR FITNESS FOR A PARTICULAR PURPOSE. See
* the AFFERO GNU General Public License for the complete license terms.
*
* You should have received a copy of the AFFERO GNU General Public License
* along with equi_join.  If not, see <http://www.gnu.org/licenses/agpl-3.0.html>
*
* END_COPYRIGHT
*/

#include <query/Operator.h>
#include <array/SortArray.h>

#include "ArrayIO.h"
#include "JoinHashTable.h"

namespace scidb
{

using namespace std;
using namespace equi_join;

class PhysicalEquiJoin : public PhysicalOperator
{
public:
    PhysicalEquiJoin(string const& logicalName,
                             string const& physicalName,
                             Parameters const& parameters,
                             ArrayDesc const& schema):
         PhysicalOperator(logicalName, physicalName, parameters, schema)
    {}

    virtual bool changesDistribution(std::vector<ArrayDesc> const&) const
    {
        return true;
    }

    virtual RedistributeContext getOutputDistribution(
               std::vector<RedistributeContext> const& inputDistributions,
               std::vector< ArrayDesc> const& inputSchemas) const
    {
        return RedistributeContext(createDistribution(psUndefined), _schema.getResidency() );
    }

    size_t computeExactArraySize(shared_ptr<Array> &input, shared_ptr<Query>& query)
    {
        size_t result = 0;
        size_t const nAttrs = input->getArrayDesc().getAttributes().size();
        vector<shared_ptr<ConstArrayIterator> >aiters(nAttrs);
        for(size_t i =0; i<nAttrs; ++i)
        {
            aiters[i] = input->getConstIterator(i);
        }
        while(!aiters[0]->end())
        {
            for(size_t i =0; i<nAttrs; ++i)
            {
                result += aiters[i]->getChunk().getSize();
            }
            for(size_t i =0; i<nAttrs; ++i)
            {
                ++(*aiters[i]);
            }
        }
        return result;
    }

    size_t globalComputeExactArraySize(shared_ptr<Array> &input, shared_ptr<Query>& query)
    {
        size_t size =  computeExactArraySize(input, query);
        size_t const nInstances = query->getInstancesCount();
        InstanceID myId = query->getInstanceID();
        std::shared_ptr<SharedBuffer> buf(new MemoryBuffer(NULL, sizeof(size_t)));
        *((size_t*) buf->getData()) = size;
        for(InstanceID i=0; i<nInstances; i++)
        {
           if(i != myId)
           {
               BufSend(i, buf, query);
           }
        }
        for(InstanceID i=0; i<nInstances; i++)
        {
           if(i != myId)
           {
               buf = BufReceive(i,query);
               size_t otherInstanceSize = *((size_t*) buf->getData());
               size += otherInstanceSize;
           }
        }
        return size;
    }

    /**
     * If all nodes call this with true - return true.
     * Otherwise, return false.
     */
    bool agreeOnBoolean(bool value, shared_ptr<Query>& query)
    {
        std::shared_ptr<SharedBuffer> buf(new MemoryBuffer(NULL, sizeof(bool)));
        InstanceID myId = query->getInstanceID();
        *((bool*) buf->getData()) = value;
        for(InstanceID i=0; i<query->getInstancesCount(); i++)
        {
            if(i != myId)
            {
                BufSend(i, buf, query);
            }
        }
        for(InstanceID i=0; i<query->getInstancesCount(); i++)
        {
            if(i != myId)
            {
                buf = BufReceive(i,query);
                bool otherInstanceVal = *((bool*) buf->getData());
                value = value && otherInstanceVal;
            }
        }
        return value;
    }

    struct PreScanResult
    {
        bool finishedLeft;
        bool finishedRight;
        size_t leftSizeEstimate;
        size_t rightSizeEstimate;
        PreScanResult():
            finishedLeft(false),
            finishedRight(false),
            leftSizeEstimate(0),
            rightSizeEstimate(0)
        {}
    };

    PreScanResult localPreScan(vector< shared_ptr< Array> >& inputArrays, shared_ptr<Query> const& query, Settings const& settings)
    {
        LOG4CXX_DEBUG(logger, "EJ starting local prescan");
        if(inputArrays[0]->getSupportedAccess() == Array::SINGLE_PASS)
        {
            LOG4CXX_DEBUG(logger, "EJ ensuring left random access");
            inputArrays[0] = ensureRandomAccess(inputArrays[0], query);
        }
        if(inputArrays[1]->getSupportedAccess() == Array::SINGLE_PASS)
        {
            LOG4CXX_DEBUG(logger, "EJ ensuring right random access");
            inputArrays[1] = ensureRandomAccess(inputArrays[1], query); //TODO: well, after this nasty thing we can know the exact size
        }
        ArrayDesc const& leftDesc  = inputArrays[0]->getArrayDesc();
        ArrayDesc const& rightDesc = inputArrays[1]->getArrayDesc();
        size_t leftCellSize  = PhysicalBoundaries::getCellSizeBytes(makePreTupledSchema<LEFT> (settings, query).getAttributes());
        size_t rightCellSize = PhysicalBoundaries::getCellSizeBytes(makePreTupledSchema<RIGHT>(settings, query).getAttributes());
        shared_ptr<ConstArrayIterator> laiter = inputArrays[0]->getConstIterator(leftDesc.getAttributes().size()-1);
        shared_ptr<ConstArrayIterator> raiter = inputArrays[1]->getConstIterator(rightDesc.getAttributes().size()-1);
        size_t leftSize =0, rightSize =0;
        size_t const threshold = settings.getHashJoinThreshold();
        while(leftSize < threshold && rightSize < threshold && !laiter->end() && !raiter->end())
        {
            leftSize  += laiter->getChunk().count() * leftCellSize;
            rightSize += raiter->getChunk().count() * rightCellSize;
            ++(*laiter);
            ++(*raiter);
        }
        if(laiter->end())
        {
            while(!raiter->end() && rightSize<threshold)
            {
                rightSize += raiter->getChunk().count() * rightCellSize;
                ++(*raiter);
            }
        }
        if(raiter->end())
        {
            while(!laiter->end() && leftSize<threshold)
            {
                leftSize += laiter->getChunk().count() * leftCellSize;
                ++(*laiter);
            }
        }
        PreScanResult result;
        if(laiter->end())
        {
            result.finishedLeft=true;
        }
        if(raiter->end())
        {
            result.finishedRight=true;
        }
        result.leftSizeEstimate =leftSize;
        result.rightSizeEstimate=rightSize;
        LOG4CXX_DEBUG(logger, "EJ prescan complete leftFinished "<<result.finishedLeft<<" rightFinished "<< result.finishedRight<<" leftSize "<<result.leftSizeEstimate
                              <<" rightSize "<<result.rightSizeEstimate);
        return result;
    }

    void globalPreScan(vector< shared_ptr< Array> >& inputArrays, shared_ptr<Query>& query, Settings const& settings,
                       size_t& leftFinished, size_t& rightFinished, size_t& leftSizeEst, size_t& rightSizeEst)
    {
        leftFinished = 0;
        rightFinished = 0;
        leftSizeEst = 0;
        rightSizeEst = 0;
        PreScanResult localResult = localPreScan(inputArrays, query, settings);
        if(localResult.finishedLeft)
        {
            leftFinished++;
        }
        if(localResult.finishedRight)
        {
            rightFinished++;
        }
        leftSizeEst+=localResult.leftSizeEstimate;
        rightSizeEst+=localResult.rightSizeEstimate;
        shared_ptr<SharedBuffer> buf(new MemoryBuffer(NULL, sizeof(PreScanResult)));
        InstanceID myId = query->getInstanceID();
        *((PreScanResult*) buf->getData()) = localResult;
        for(InstanceID i=0; i<query->getInstancesCount(); i++)
        {
            if(i != myId)
            {
                BufSend(i, buf, query);
            }
        }
        for(InstanceID i=0; i<query->getInstancesCount(); i++)
        {
            if(i != myId)
            {
                buf = BufReceive(i,query);
                PreScanResult otherInstanceResult = *((PreScanResult*) buf->getData());
                if(otherInstanceResult.finishedLeft)
                {
                    leftFinished++;
                }
                if(otherInstanceResult.finishedRight)
                {
                    rightFinished++;
                }
                leftSizeEst+=otherInstanceResult.leftSizeEstimate;
                rightSizeEst+=otherInstanceResult.rightSizeEstimate;
            }
        }
    }

    Settings::algorithm pickAlgorithm(vector< shared_ptr< Array> >& inputArrays, shared_ptr<Query>& query, Settings const& settings)
    {
        if(settings.algorithmSet()) //user override
        {
            return settings.getAlgorithm();
        }
        size_t const nInstances = query->getInstancesCount();
        size_t const hashJoinThreshold = settings.getHashJoinThreshold();
        bool leftMaterialized = agreeOnBoolean(inputArrays[0]->isMaterialized(), query);
        size_t exactLeftSize  = leftMaterialized ? globalComputeExactArraySize(inputArrays[0], query) : -1;
        LOG4CXX_DEBUG(logger, "EJ left materialized "<<leftMaterialized<< " exact left size "<<exactLeftSize);
        if(leftMaterialized && exactLeftSize < hashJoinThreshold)
        {
            return Settings::HASH_REPLICATE_LEFT;
        }
        bool rightMaterialized = agreeOnBoolean(inputArrays[1]->isMaterialized(), query);
        size_t exactRightSize = rightMaterialized ? globalComputeExactArraySize(inputArrays[1], query) : -1;
        LOG4CXX_DEBUG(logger, "EJ right materialized "<<rightMaterialized<< " exact right size "<<exactRightSize);
        if(rightMaterialized && exactRightSize < hashJoinThreshold)
        {
            return Settings::HASH_REPLICATE_RIGHT;
        }
        if(leftMaterialized && rightMaterialized)
        {
            return exactLeftSize < exactRightSize ? Settings::MERGE_LEFT_FIRST : Settings::MERGE_RIGHT_FIRST;
        }
        size_t leftArraysFinished =0;
        size_t rightArraysFinished=0;
        size_t leftSizeEst = 0;
        size_t rightSizeEst =0;
        globalPreScan(inputArrays, query, settings, leftArraysFinished, rightArraysFinished, leftSizeEst, rightSizeEst);
        LOG4CXX_DEBUG(logger, "EJ global prescan complete leftFinished "<<leftArraysFinished<<" rightFinished "<< rightArraysFinished<<" leftSizeEst "<<leftSizeEst<<
                      " rightSizeEst "<<rightSizeEst);
        if(leftArraysFinished == nInstances && leftSizeEst < hashJoinThreshold)
        {
            return Settings::HASH_REPLICATE_LEFT;
        }
        if(rightArraysFinished == nInstances && rightSizeEst < hashJoinThreshold)
        {
            return Settings::HASH_REPLICATE_RIGHT;
        }
        //~~~ I dunno, Richard Parker, what do you think?
        return leftArraysFinished < rightArraysFinished ? Settings::MERGE_RIGHT_FIRST : Settings::MERGE_LEFT_FIRST;
    }

    template <Handedness which, ReadArrayType arrayType>
    void readIntoTable(shared_ptr<Array> & array, JoinHashTable& table, Settings const& settings, ChunkFilter<which>* chunkFilterToPopulate = NULL)
    {
        ArrayReader<which, arrayType> reader(array, settings);
        while(!reader.end())
        {
            vector<Value const*> const& tuple = reader.getTuple();
            if(chunkFilterToPopulate)
            {
                chunkFilterToPopulate->addTuple(tuple);
            }
            table.insert(tuple);
            reader.next();
        }
    }

    template <Handedness which, ReadArrayType arrayType>
    shared_ptr<Array> arrayToTableJoin(shared_ptr<Array>& array, JoinHashTable& table, shared_ptr<Query>& query,
                                       Settings const& settings, ChunkFilter<which> const* chunkFilter = NULL)
    {
        //handedness LEFT means the LEFT array is in table so this reads in reverse
        ArrayReader<which == LEFT ? RIGHT : LEFT, arrayType> reader(array, settings, chunkFilter, NULL);
        ArrayWriter<WRITE_OUTPUT> result(settings, query, _schema);
        JoinHashTable::const_iterator iter = table.getIterator();
        while(!reader.end())
        {
            vector<Value const*> const& tuple = reader.getTuple();
            iter.find(tuple);
            while(!iter.end() && iter.atKeys(tuple))
            {
                Value const* tablePiece = iter.getTuple();
                if(which == LEFT)
                {
                    result.writeTuple(tablePiece, tuple);
                }
                else
                {
                    result.writeTuple(tuple, tablePiece);
                }
                iter.nextAtHash();
            }
            reader.next();
        }
        return result.finalize();
    }

    template <Handedness which>
    shared_ptr<Array> replicationHashJoin(vector< shared_ptr< Array> >& inputArrays, shared_ptr<Query> query, Settings const& settings)
    {
        shared_ptr<Array> redistributed = (which == LEFT ? inputArrays[0] : inputArrays[1]);
        redistributed = redistributeToRandomAccess(redistributed, createDistribution(psReplication), ArrayResPtr(), query);
        ArenaPtr operatorArena = this->getArena();
        ArenaPtr hashArena(newArena(Options("").resetting(true).threading(false).pagesize(8 * 1024 * 1204).parent(operatorArena)));
        JoinHashTable table(settings, hashArena, which == LEFT ? settings.getLeftTupleSize() : settings.getRightTupleSize());
        ChunkFilter <which> filter(settings, inputArrays[0]->getArrayDesc(), inputArrays[1]->getArrayDesc());
        readIntoTable<which, READ_INPUT> (redistributed, table, settings, &filter);
        return arrayToTableJoin<which, READ_INPUT>( which == LEFT ? inputArrays[1]: inputArrays[0], table, query, settings, &filter);
    }

    template <Handedness which>
    shared_ptr<Array> readIntoPreSort(shared_ptr<Array> & inputArray, shared_ptr<Query>& query, Settings const& settings,
                                      ChunkFilter<which>* chunkFilterToGenerate, ChunkFilter<which == LEFT ? RIGHT : LEFT> const* chunkFilterToApply,
                                      BloomFilter* bloomFilterToGenerate,        BloomFilter const* bloomFilterToApply)
    {
        ArrayReader<which, READ_INPUT> reader(inputArray, settings, chunkFilterToApply, bloomFilterToApply);
        ArrayWriter<WRITE_TUPLED> writer(settings, query, makePreTupledSchema<which>(settings, query));
        size_t const hashMod = settings.getNumHashBuckets();
        vector<char> hashBuf(64);
        size_t const numKeys = settings.getNumKeys();
        Value hashVal;
        while(!reader.end())
        {
            vector<Value const*> const& tuple = reader.getTuple();
            if(chunkFilterToGenerate)
            {
                chunkFilterToGenerate->addTuple(tuple);
            }
            if(bloomFilterToGenerate)
            {
                bloomFilterToGenerate->addTuple(tuple, numKeys);
            }
            hashVal.setUint32( JoinHashTable::hashKeys(tuple, numKeys, hashBuf) % hashMod);
            writer.writeTupleWithHash(tuple, hashVal);
            reader.next();
        }
        return writer.finalize();
    }

    shared_ptr<Array> sortArray(shared_ptr<Array> & inputArray, shared_ptr<Query>& query, Settings const& settings)
    {
        SortingAttributeInfos sortingAttributeInfos(settings.getNumKeys() + 1); //plus hash
        sortingAttributeInfos[0].columnNo = inputArray->getArrayDesc().getAttributes(true).size()-1;
        sortingAttributeInfos[0].ascent = true;
        for(size_t k=0; k<settings.getNumKeys(); ++k)
        {
            sortingAttributeInfos[k+1].columnNo = k;
            sortingAttributeInfos[k+1].ascent = true;
        }
        SortArray sorter(inputArray->getArrayDesc(), _arena, false, settings.getChunkSize());
        shared_ptr<TupleComparator> tcomp(make_shared<TupleComparator>(sortingAttributeInfos, inputArray->getArrayDesc()));
        return sorter.getSortedArray(inputArray, query, tcomp);
    }

    template <Handedness which>
    shared_ptr<Array> sortedToPreSg(shared_ptr<Array> & inputArray, shared_ptr<Query>& query, Settings const& settings)
    {
        ArrayWriter<WRITE_SPLIT_ON_HASH> writer(settings, query, makePreTupledSchema<which>(settings, query));
        ArrayReader<which, READ_TUPLED> reader(inputArray, settings);
        while(!reader.end())
        {
            writer.writeTuple(reader.getTuple());
            reader.next();
        }
        return writer.finalize();
    }

    shared_ptr<Array> localSortedMergeJoin(shared_ptr<Array>& leftSorted, shared_ptr<Array>& rightSorted, shared_ptr<Query>& query, Settings const& settings)
    {
        ArrayWriter<WRITE_OUTPUT> output(settings, query, _schema);
        vector<AttributeComparator> const& comparators = settings.getKeyComparators();
        size_t const numKeys = settings.getNumKeys();
        ArrayReader<LEFT, READ_SORTED>  leftCursor (leftSorted,  settings);
        ArrayReader<RIGHT, READ_SORTED> rightCursor(rightSorted, settings);
        if(leftCursor.end() || rightCursor.end())
        {
            return output.finalize();
        }
        vector<Value> previousLeftTuple(numKeys);
        Coordinate previousRightIdx = -1;
        vector<Value const*> const* leftTuple = &(leftCursor.getTuple());
        vector<Value const*> const* rightTuple = &(rightCursor.getTuple());
        size_t leftTupleSize = settings.getLeftTupleSize();
        size_t rightTupleSize = settings.getRightTupleSize();
        while(!leftCursor.end() && !rightCursor.end())
        {
            uint32_t leftHash = ((*leftTuple)[leftTupleSize])->getUint32();
            uint32_t rightHash =((*rightTuple)[rightTupleSize])->getUint32();
            while(rightHash < leftHash && !rightCursor.end())
            {
                rightCursor.next();
                if(!rightCursor.end())
                {
                    rightTuple = &(rightCursor.getTuple());
                    rightHash =((*rightTuple)[rightTupleSize])->getUint32();
                }
            }
            if(rightHash > leftHash)
            {
                leftCursor.next();
                if(!leftCursor.end())
                {
                    leftTuple = &(leftCursor.getTuple());
                }
                continue;
            }
            if(rightCursor.end())
            {
                break;
            }
            while (!rightCursor.end() && rightHash == leftHash && JoinHashTable::keysLess(*rightTuple, *leftTuple, comparators, numKeys) )
            {
                rightCursor.next();
                if(!rightCursor.end())
                {
                    rightTuple = &(rightCursor.getTuple());
                    rightHash =((*rightTuple)[rightTupleSize])->getUint32();
                }
            }
            if(rightCursor.end())
            {
                break;
            }
            if(rightHash > leftHash)
            {
                leftCursor.next();
                if(!leftCursor.end())
                {
                    leftTuple = &(leftCursor.getTuple());
                }
                continue;
            }
            previousRightIdx = rightCursor.getIdx();
            bool first = true;
            while(!rightCursor.end() && rightHash == leftHash && JoinHashTable::keysEqual(*leftTuple, *rightTuple, numKeys))
            {
                if(first)
                {
                    for(size_t i=0; i<numKeys; ++i)
                    {
                        previousLeftTuple[i] = *((*leftTuple)[i]);
                    }
                    first = false;
                }
                output.writeTuple(*leftTuple, *rightTuple);
                rightCursor.next();
                if(!rightCursor.end())
                {
                    rightTuple = &rightCursor.getTuple();
                    rightHash =((*rightTuple)[rightTupleSize])->getUint32();
                }
            }
            leftCursor.next();
            if(!leftCursor.end())
            {
                leftTuple = &leftCursor.getTuple();
                if(JoinHashTable::keysEqual( &(previousLeftTuple[0]), *leftTuple, numKeys) && !first)
                {
                    rightCursor.setIdx(previousRightIdx);
                    rightTuple = &rightCursor.getTuple();
                }
            }
        }
        return output.finalize();
    }

    template <Handedness which>
    shared_ptr<Array> globalMergeJoin(vector< shared_ptr< Array> >& inputArrays, shared_ptr<Query> query, Settings const& settings)
    {
        shared_ptr<Array>& first = (which == LEFT ? inputArrays[0] : inputArrays[1]);
        ChunkFilter <which> chunkFilter(settings, inputArrays[0]->getArrayDesc(), inputArrays[1]->getArrayDesc());
        BloomFilter bloomFilter(settings.getBloomFilterSize());
        first = readIntoPreSort<which>(first, query, settings, &chunkFilter, NULL, &bloomFilter, NULL);
        first = sortArray(first, query, settings);
        first = sortedToPreSg<which>(first, query, settings);
        first = redistributeToRandomAccess(first,createDistribution(psByRow),query->getDefaultArrayResidency(), query, true);
        chunkFilter.globalExchange(query);
        bloomFilter.globalExchange(query);
        shared_ptr<Array>& second = (which == LEFT ? inputArrays[1] : inputArrays[0]);
        second = readIntoPreSort<(which == LEFT ? RIGHT : LEFT)>(second, query, settings, NULL, &chunkFilter, NULL, &bloomFilter);
        second = sortArray(second, query, settings);
        second = sortedToPreSg<(which == LEFT ? RIGHT : LEFT)>(second, query, settings);
        second = redistributeToRandomAccess(second,createDistribution(psByRow),query->getDefaultArrayResidency(), query, true);
        size_t const firstSize  = computeExactArraySize(first, query);
        size_t const secondSize = computeExactArraySize(first, query);
        LOG4CXX_DEBUG(logger, "EJ merge after SG first size "<<firstSize<<" second size "<<secondSize);
        if (firstSize < settings.getHashJoinThreshold())
        {
            LOG4CXX_DEBUG(logger, "EJ merge rehashing first");
            ArenaPtr operatorArena = this->getArena();
            ArenaPtr hashArena(newArena(Options("").resetting(true).threading(false).pagesize(8 * 1024 * 1204).parent(operatorArena)));
            JoinHashTable table(settings, hashArena, which == LEFT ? settings.getLeftTupleSize() : settings.getRightTupleSize());
            readIntoTable<which, READ_TUPLED> (first, table, settings);
            return arrayToTableJoin<which, READ_TUPLED>( second, table, query, settings);
        }
        else if(secondSize < settings.getHashJoinThreshold())
        {
            LOG4CXX_DEBUG(logger, "EJ merge rehashing second");
            ArenaPtr operatorArena = this->getArena();
            ArenaPtr hashArena(newArena(Options("").resetting(true).threading(false).pagesize(8 * 1024 * 1204).parent(operatorArena)));
            JoinHashTable table(settings, hashArena, which == LEFT ? settings.getRightTupleSize() : settings.getLeftTupleSize());
            readIntoTable<(which == LEFT ? RIGHT : LEFT), READ_TUPLED> (second, table, settings);
            return arrayToTableJoin<(which == LEFT ? RIGHT : LEFT), READ_TUPLED>( first, table, query, settings);
        }
        else
        {
            LOG4CXX_DEBUG(logger, "EJ merge sorted");
            first = sortArray(first, query, settings);
            second= sortArray(second, query, settings);
            return which == LEFT ? localSortedMergeJoin(first, second, query, settings) :  localSortedMergeJoin(second, first, query, settings);
        }
    }

    shared_ptr< Array> execute(vector< shared_ptr< Array> >& inputArrays, shared_ptr<Query> query)
    {
        vector<ArrayDesc const*> inputSchemas(2);
        inputSchemas[0] = &inputArrays[0]->getArrayDesc();
        inputSchemas[1] = &inputArrays[1]->getArrayDesc();
        Settings settings(inputSchemas, _parameters, false, query);
        Settings::algorithm algo = pickAlgorithm(inputArrays, query, settings);
        if(algo == Settings::HASH_REPLICATE_LEFT)
        {
            LOG4CXX_DEBUG(logger, "EJ running hash_replicate_left");
            return replicationHashJoin<LEFT>(inputArrays, query, settings);
        }
        else if (algo == Settings::HASH_REPLICATE_RIGHT)
        {
            LOG4CXX_DEBUG(logger, "EJ running hash_replicate_right");
            return replicationHashJoin<RIGHT>(inputArrays, query, settings);
        }
        else if (algo == Settings::MERGE_LEFT_FIRST)
        {
            LOG4CXX_DEBUG(logger, "EJ running merge_left_first");
            return globalMergeJoin<LEFT>(inputArrays, query, settings);
        }
        else
        {
            LOG4CXX_DEBUG(logger, "EJ running merge_right_first");
            return globalMergeJoin<RIGHT>(inputArrays, query, settings);
        }
    }
};

REGISTER_PHYSICAL_OPERATOR_FACTORY(PhysicalEquiJoin, "equi_join", "physical_equi_join");
} //namespace scidb
