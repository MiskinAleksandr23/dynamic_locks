/*
 *  Copyright 2018 Kjell Winblad (kjellwinblad@gmail.com, http://winsh.me)
 *
 *  This file is part of JavaRQBench
 *
 *  catrees is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  catrees is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with catrees.  If not, see <http://www.gnu.org/licenses/>.
 */

package se.uu.collection;
import java.util.LinkedList;
import java.util.concurrent.locks.*;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.*;
import java.io.*;








 
public class CATreeMapSTDR<K, V> extends AbstractMap<K,V> implements RangeQueryMap<K, V>, RangeUpdateMap<K, V>{

    private volatile Object root = new AVLTreeMapSTD<K, V>();
    private final Comparator<? super K> comparator;
    // ====== FOR DEBUGING ======
    @SuppressWarnings("unused")
	private final static boolean DEBUG = false;
    // ==========================

    static private final class RouteNode{
        volatile Object left;
        volatile Object right;
        final Object key;
        final ReentrantLock lock = new ReentrantLock();
        boolean valid = true;
        public RouteNode(Object key, Object left, Object right){
            this.key = key;
            this.left = left;
            this.right = right;
        }
        public String toString(){
            return "R(" + key + ")";
        }
    }

    //==== Functions for debuging and testing

    void printDotHelper(Object n, PrintStream writeTo, int level){
        try{
            if(n instanceof RouteNode){
                RouteNode node = (RouteNode)n;
                //LEFT
                writeTo.print("\"" + node + level+" \"");
                writeTo.print(" -> ");
                writeTo.print("\"" + node.left + (level +1)+" \"");
                writeTo.println(";");
                //RIGHT
                writeTo.print("\"" + node + level+" \"");
                writeTo.print(" -> ");
                writeTo.print("\"" + node.right + (level +1)+" \"");
                writeTo.println(";");

                printDotHelper(node.left, writeTo, level +1);
                printDotHelper(node.right, writeTo, level +1);
            }
        }catch(Exception e){
            e.printStackTrace();
        }
    }


    void printDot(Object node, String fileName){
        try{
            lockAll();
            Process p = new ProcessBuilder("dot", "-Tsvg")
                .redirectOutput(ProcessBuilder.Redirect.to(new File(fileName + ".svg")))
                .start();
            PrintStream writeTo = new PrintStream(p.getOutputStream());
            writeTo.print("digraph G{\n");
            writeTo.print("  graph [ordering=\"out\"];\n");
            printDotHelper(node, writeTo, 0);
            writeTo.print("}\n");
            writeTo.close();
            p.waitFor();
            unlockAll();
        }catch(Exception e){
            e.printStackTrace();
        }
    }


    private int numberOfRouteNodes(Object currentNode){
        if(currentNode == null){
            return 0;
        }else{
            if(currentNode instanceof RouteNode){
                RouteNode r = (RouteNode)currentNode;
                int sizeSoFar = numberOfRouteNodes(r.left);
                return  1 + sizeSoFar + numberOfRouteNodes(r.right);
            }else {
                return 0;
            }
        }
    }


    public int numberOfRouteNodes(){
	return numberOfRouteNodes(root);
    }

    //=== End of debug functions ==================


    //=== Constructors ============================

    public CATreeMapSTDR() {
        comparator = null;
    }

    public CATreeMapSTDR(Comparator<? super K> comparator) {
        this.comparator = comparator;
    }


    //=== Public functions and helper functions ===


    //=== Sorted Set Functions ====================


    private int sizeHelper(Object currentNode){
        if(currentNode == null){
            return 0;
        }else{
            if(currentNode instanceof RouteNode){
                RouteNode r = (RouteNode)currentNode;
                int sizeSoFar = sizeHelper(r.left);
                return sizeSoFar + sizeHelper(r.right);
            }else {
                @SuppressWarnings("unchecked")
                AVLTreeMapSTD<K,V> b = (AVLTreeMapSTD<K,V>)currentNode;
                return b.size();
            }
        }
    }

    public int size(){
        lockAll();
        int size = sizeHelper(root);
        unlockAll();
        return size;
    }

    public boolean isEmpty(){
        return size() == 0;
    }

    public boolean containsKey(Object key){
        return get(key) != null;
    }

    final private Object getBaseNodeUsingComparator(Object keyParam){
        Object currNode = root;
        @SuppressWarnings("unchecked")
        K key = (K)keyParam;
        while (currNode instanceof RouteNode) {
            RouteNode currNodeR = (RouteNode)currNode;
            @SuppressWarnings("unchecked")
            K routeKey = (K)(currNodeR.key);
            if (comparator.compare(key, routeKey) < 0) {
                currNode = currNodeR.left;
            }else {
                currNode = currNodeR.right;
            }
        }
        return currNode;
    }

    final private Object getBaseNode(Object keyParam){
        Object currNode = root;
        if(comparator != null){
            return getBaseNodeUsingComparator(keyParam);
        }else{
            @SuppressWarnings("unchecked")
            Comparable<? super K> key = (Comparable<? super K>) keyParam;
            while (currNode instanceof RouteNode) {
                RouteNode currNodeR = (RouteNode)currNode;
                @SuppressWarnings("unchecked")
                K routeKey = (K)(currNodeR.key);
                if (key.compareTo(routeKey) < 0) {
                    currNode = currNodeR.left;
                } else {
                    currNode = currNodeR.right;
                }
            }
            return currNode;
        }
    }

    final private void highContentionSplit(AVLTreeMapSTD<K, V> baseNode){
        if(baseNode.getRoot() == null || 
           (baseNode.getRoot().getLeft() == null &&
            baseNode.getRoot().getRight() == null)){
            baseNode.resetStatistics();//Fast path out if nrOfElem <= 1
            return;
        }

        RouteNode parent = (RouteNode)baseNode.getParent();
        Object[] writeBackSplitKey = new Object[1];
        @SuppressWarnings("unchecked")
        SplitableAndJoinableMap<K,V>[] writeBackRightTree = new SplitableAndJoinableMap[1];
        AVLTreeMapSTD<K,V> leftTree = (AVLTreeMapSTD<K,V>)baseNode.split(writeBackSplitKey, writeBackRightTree);
        if(leftTree == null){
            baseNode.resetStatistics();
            return;
        }
        @SuppressWarnings("unchecked")
        K splitKey = (K)writeBackSplitKey[0];
        AVLTreeMapSTD<K,V> rightTree = (AVLTreeMapSTD<K,V>)writeBackRightTree[0];
        RouteNode newRoute = new RouteNode(splitKey, leftTree, rightTree);
        leftTree.setParent(newRoute);
        rightTree.setParent(newRoute);
        if (parent == null) {
            root = newRoute;
        }else {
            if (parent.left == baseNode){
                parent.left = newRoute;
            }else{
                parent.right = newRoute;
            }
        }
        baseNode.invalidate();
    }

    final private AVLTreeMapSTD<K,V> leftmostBaseNode(Object node){
        Object currentNode = node;
        while(currentNode instanceof RouteNode){
            RouteNode r = (RouteNode)currentNode;
            currentNode = r.left;
        }
        @SuppressWarnings("unchecked")
        AVLTreeMapSTD<K,V> toReturn = (AVLTreeMapSTD<K,V>)currentNode;
        return toReturn;
    }

    final private AVLTreeMapSTD<K,V> rightmostBaseNode(Object node){
        Object currentNode = node;
        while(currentNode instanceof RouteNode){
            RouteNode r = (RouteNode)currentNode;
            currentNode = r.right;
        }
        @SuppressWarnings("unchecked")
        AVLTreeMapSTD<K,V> toReturn = (AVLTreeMapSTD<K,V>)currentNode;
        return toReturn;
    }

    final private RouteNode parentOfUsingComparator(RouteNode node){
        @SuppressWarnings("unchecked")
        K key = (K)node.key;
        Object prevNode = null;
        Object currNode = root;

        while (currNode != node) {
            RouteNode currNodeR = (RouteNode)currNode;
            @SuppressWarnings("unchecked")
            K routeKey = (K)(currNodeR.key);
            prevNode = currNode;
            if (comparator.compare(key, routeKey) < 0) {
                currNode = currNodeR.left;
            } else {
                currNode = currNodeR.right;
            }
        }
        return (RouteNode)prevNode;

    }

    final private RouteNode parentOf(RouteNode node){
        if(comparator != null){
            return parentOfUsingComparator(node);
        }else{
            @SuppressWarnings("unchecked")
            Comparable<? super K> key = (Comparable<? super K>) node.key;
            Object prevNode = null;
            Object currNode = root;
            while (currNode != node) {
                RouteNode currNodeR = (RouteNode)currNode;
                @SuppressWarnings("unchecked")
                K routeKey = (K)(currNodeR.key);
                prevNode = currNode;
                if (key.compareTo(routeKey) < 0) {
                    currNode = currNodeR.left;
                } else {
                    currNode = currNodeR.right;
                }
            }
            return (RouteNode)prevNode;
        }
    }

    final private void lowContentionJoin(AVLTreeMapSTD<K, V> baseNode){
        RouteNode parent = (RouteNode)baseNode.getParent();
        if(parent == null){
            baseNode.resetStatistics();
        }else if (parent.left == baseNode) {
            AVLTreeMapSTD<K,V> neighborBase = leftmostBaseNode(parent.right);
            if (!neighborBase.tryLock()) {
                baseNode.resetStatistics();
                return;
            } else if (!neighborBase.isValid()) {
                neighborBase.unlock();
                baseNode.resetStatistics();
                return;
            } else {
                AVLTreeMapSTD<K,V> newNeighborBase = (AVLTreeMapSTD<K,V>)baseNode.join(neighborBase);
                parent.lock.lock();
                RouteNode gparent = null; // gparent = grandparent
                do {
                    if (gparent != null){
                        gparent.lock.unlock();
                    }
                    gparent = parentOf(parent);
                    if (gparent != null){
                        gparent.lock.lock();
                    }
                } while (gparent != null && !gparent.valid);
                if (gparent == null) {
                    root = parent.right;
                } else if(gparent.left == parent){
                    gparent.left = parent.right;
                } else {
                    gparent.right = parent.right;
                }
                parent.valid = false;
                parent.lock.unlock();
                if (gparent != null){
                    gparent.lock.unlock();
                }
                //Unlink is done!
                //Put in joined base node
                RouteNode neighborBaseParent = null;
                if(parent.right == neighborBase){
                    neighborBaseParent = gparent;
                }else{
                    neighborBaseParent = (RouteNode)neighborBase.getParent();
                }
                newNeighborBase.setParent(neighborBaseParent);
                if(neighborBaseParent == null){
                    root = newNeighborBase;
                } else if (neighborBaseParent.left == neighborBase) {
                    neighborBaseParent.left = newNeighborBase;
                } else {
                    neighborBaseParent.right = newNeighborBase;
                }
                neighborBase.invalidate();
                neighborBase.unlock();
                baseNode.invalidate();
            }
        } else { /* This case is symmetric to the previous one */
            AVLTreeMapSTD<K,V> neighborBase = rightmostBaseNode(parent.left);//ff
            if (!neighborBase.tryLock()) {//ff
                baseNode.resetStatistics();//ff
            } else if (!neighborBase.isValid()) {//ff
                neighborBase.unlock();//ff
                baseNode.resetStatistics();//ff
            } else {
                //                System.out.println("R" + baseNode + " " + neighborBase);
                AVLTreeMapSTD<K,V> newNeighborBase = (AVLTreeMapSTD<K,V>)neighborBase.join(baseNode);//ff
                parent.lock.lock();//ff
                RouteNode gparent = null; // gparent = grandparent //ff
                do {//ff
                    if (gparent != null){//ff
                        gparent.lock.unlock();//ff
                    }//ff
                    gparent = parentOf(parent);//ff
                    if (gparent != null){//ff
                        gparent.lock.lock();//ff
                    }//ff
                } while (gparent != null && !gparent.valid);//ff
                if (gparent == null) {//ff
                    root = parent.left;//ff
                } else if(gparent.left == parent){//ff
                    gparent.left = parent.left;//ff
                } else {//ff
                    gparent.right = parent.left;//ff
                }//ff
                parent.valid = false;
                parent.lock.unlock();//ff
                if (gparent != null){//ff
                    gparent.lock.unlock();//ff
                }//ff
                RouteNode neighborBaseParent = null;
                if(parent.left == neighborBase){
                    neighborBaseParent = gparent;
                }else{
                    neighborBaseParent = (RouteNode)neighborBase.getParent();
                }
                newNeighborBase.setParent(neighborBaseParent);//ff
                if(neighborBaseParent == null){//ff
                    root = newNeighborBase;//ff
                } else if (neighborBaseParent.left == neighborBase) {//ff
                    neighborBaseParent.left = newNeighborBase;//ff
                } else {//ff
                    neighborBaseParent.right = newNeighborBase;//ff
                }//ff
                neighborBase.invalidate();//ff
                neighborBase.unlock();//ff
                baseNode.invalidate();//ff
            }
        }
    }

    private final void adaptIfNeeded(AVLTreeMapSTD<K,V> baseNode){
        if (baseNode.isHighContentionLimitReached()){
            highContentionSplit(baseNode);
        } else if (baseNode.isLowContentionLimitReached()) {
            lowContentionJoin(baseNode);
        }
    }

    public V get(Object key){
        while(true){
            @SuppressWarnings("unchecked")
            AVLTreeMapSTD<K,V> baseNode = (AVLTreeMapSTD<K,V>)getBaseNode(key);
            //First do an optimistic attempt
            long optimisticReadToken = baseNode.getOptimisticReadToken();
            if(0L != optimisticReadToken
               && baseNode.isValid()){
                V result = baseNode.get(key);
                if(baseNode.validateOptimisticReadToken(optimisticReadToken)){
                    return result;
                }
            }
            //System.err.println("FAILED GET");
            //Optemistic attempt failed, do the normal approach
            baseNode.readLock();
            baseNode.addToContentionStatistics();//Because the optimistic attempt failed
            //Check if valid
            if (baseNode.isValid() == false) {
                baseNode.readUnlock();
                continue; // retry
            }          
            //Do the operation
            V result = baseNode.get(key);
            baseNode.readUnlock();
            return result;
        }
    }

    public V put(K key, V value){
        while(true){
            @SuppressWarnings("unchecked")
            AVLTreeMapSTD<K, V> baseNode = (AVLTreeMapSTD<K, V>)getBaseNode(key);
            baseNode.lock();
            //Check if valid
            if (!baseNode.isValid()) {
                baseNode.unlock();
                continue; // retry
            }
            //Do the operation
            V result = baseNode.put(key, value);
            adaptIfNeeded(baseNode);
            baseNode.unlock();
            return result;
        }
    }


    public V putIfAbsent(K key, V value){
        while(true){
            @SuppressWarnings("unchecked")
            AVLTreeMapSTD<K, V> baseNode = (AVLTreeMapSTD<K, V>)getBaseNode(key);
            baseNode.lock();
            //Check if valid
            if (!baseNode.isValid()) {
                baseNode.unlock();
                continue; // retry
            }
            //Do the operation
            V result = baseNode.putIfAbsent(key, value);
            adaptIfNeeded(baseNode);
            baseNode.unlock();
            return result;
        }
    }


    public V remove(Object key){
        while(true){
            @SuppressWarnings("unchecked")
            AVLTreeMapSTD<K,V> baseNode = (AVLTreeMapSTD<K,V>)getBaseNode(key);
            baseNode.lock();
            //Check if valid
            if (baseNode.isValid() == false) {
                baseNode.unlock();
                continue; // retry
            }
            //Do the operation
            V result = baseNode.remove(key);
            adaptIfNeeded(baseNode);
            baseNode.unlock();
            return result;
        }
    }

    public void clear(){
        lockAll();
        root = new AVLTreeMapSTD<K, V>();
        unlockAll();
    }


    private void lockAllHelper(Object currentNode, LinkedList<AVLTreeMapSTD<K, V>> linkedList){
        try {
            if(currentNode != null){
                if(currentNode instanceof RouteNode){
                    RouteNode r = (RouteNode)currentNode;
                    lockAllHelper(r.left, linkedList);
                    lockAllHelper(r.right, linkedList);
                }else {
                    @SuppressWarnings("unchecked")
					AVLTreeMapSTD<K,V> b = (AVLTreeMapSTD<K,V>)currentNode;
                    b.lock();
                    if(b.isValid()){
                        linkedList.addLast(b);
                    }else{
                        //Retry
                        b.unlock();
                        for(AVLTreeMapSTD<K,V> m : linkedList){
                            m.unlock();
                        }
                        throw new RuntimeException();
                    }
                }
            }
        } catch (RuntimeException e){
            //Retry
            lockAllHelper(root, new LinkedList<AVLTreeMapSTD<K,V>>());
        }
    }


    private void unlockAllHelper(Object currentNode) {
        if(currentNode != null){
            if(currentNode instanceof RouteNode) {
                RouteNode b = (RouteNode)currentNode;
                unlockAllHelper(b.left);
                unlockAllHelper(b.right);
            } else {
                @SuppressWarnings("unchecked")
				AVLTreeMapSTD<K,V> b = (AVLTreeMapSTD<K,V>)currentNode;
                b.unlock();
            }
        }
    }

    private void lockAll(){
        lockAllHelper(root, new LinkedList<AVLTreeMapSTD<K,V>>());
    }

    private void unlockAll(){
        unlockAllHelper(root);
    }

    final private void addAllToList(Object currentNode, LinkedList<Map.Entry<K, V>> list){
        if(currentNode == null){
            return;
        }else{
            if(currentNode instanceof RouteNode){
                RouteNode r = (RouteNode)currentNode;
                addAllToList(r.left, list);
                addAllToList(r.right, list);
            }else {
                @SuppressWarnings("unchecked")
                AVLTreeMapSTD<K, V> b = (AVLTreeMapSTD<K, V>)currentNode;
                b.addAllToList(list);
                return;
            }
        }
    } 

    //Set<K> keySet();
    //Collection<V> values();
    public Set<Map.Entry<K, V>> entrySet(){
        LinkedList<Map.Entry<K, V>> list = new LinkedList<Map.Entry<K, V>>();
        lockAll();
        addAllToList(root, list);
        unlockAll();
        return new HashSet<Map.Entry<K, V>>(list);
    }

    //boolean equals(Object o);
    //int hashCode();

    final private Object getBaseNodeAndStackUsingComparator(Object keyParam, Stack<RouteNode> stack){
        Object currNode = root;
        @SuppressWarnings("unchecked")
        K key = (K)keyParam;
        while (currNode instanceof RouteNode) {
            RouteNode currNodeR = (RouteNode)currNode;
            stack.push(currNodeR);
            @SuppressWarnings("unchecked")
            K routeKey = (K)(currNodeR.key);
            if (comparator.compare(key, routeKey) < 0) {
                currNode = currNodeR.left;
            }else {
                currNode = currNodeR.right;
            }
        }
        return currNode;
    }

//    final private Object getBaseNodeAndStack(Object keyParam, Stack<RouteNode> stack){
//        Object currNode = root;
//        if(comparator != null){
//            return getBaseNodeAndStackUsingComparator(keyParam, stack);
//        }else{
//            @SuppressWarnings("unchecked")
//            Comparable<? super K> key = (Comparable<? super K>) keyParam;
//            while (currNode instanceof RouteNode) {
//                RouteNode currNodeR = (RouteNode)currNode;
//                stack.push(currNodeR);
//                @SuppressWarnings("unchecked")
//                K routeKey = (K)(currNodeR.key);
//                if (key.compareTo(routeKey) < 0) {
//                    currNode = currNodeR.left;
//                } else {
//                    currNode = currNodeR.right;
//                }
//            }
//            return currNode;
//        }
//    }

    @SuppressWarnings("unchecked")
	final private AVLTreeMapSTD<K, V> getBaseNodeAndStack(Object keyParam, Stack<RouteNode> stack){
        Object currNode = root;
        if(comparator != null){
            return (AVLTreeMapSTD<K, V>)getBaseNodeAndStackUsingComparator(keyParam, stack);
        }else{
            Comparable<? super K> key = (Comparable<? super K>) keyParam;
            while (currNode instanceof RouteNode) {
                RouteNode currNodeR = (RouteNode)currNode;
                stack.push(currNodeR);
                K routeKey = (K)(currNodeR.key);
                if (key.compareTo(routeKey) < 0) {
                    currNode = currNodeR.left;
                } else {
                    currNode = currNodeR.right;
                }
            }
            return (AVLTreeMapSTD<K, V>)currNode;
        }
    }
    
    final private AVLTreeMapSTD<K,V> leftmostBaseNodeAndStack(Object node, Stack<RouteNode> stack){
        Object currentNode = node;
        while(currentNode instanceof RouteNode){
            RouteNode r = (RouteNode)currentNode;
            stack.push(r);
            currentNode = r.left;
        }
        @SuppressWarnings("unchecked")
        AVLTreeMapSTD<K,V> toReturn = (AVLTreeMapSTD<K,V>)currentNode;
        return toReturn;
    }

//    final private Object getNextBaseNodeAndStack(Object baseNode, Stack<RouteNode> stack, Stack<RouteNode> nextStack){
//        RouteNode top = stack.top();
//        if(top == null){
//            return null;
//        }if(top.valid && top.left == baseNode){
//            nextStack.copyStateFrom(stack);
//            return leftmostBaseNodeAndStack(top.right, nextStack);
//        }else{
//            stack.pop();
//            RouteNode prevTop = top;
//            top = stack.top();
//            while(top!= null && (!top.valid || top.right == prevTop)){
//                stack.pop();
//                prevTop = top;
//                top = stack.top();
//            }
//            if(top == null){
//                return null;
//            }else{
//                nextStack.copyStateFrom(stack);
//                return leftmostBaseNodeAndStack(top.right, nextStack);
//            }
//        }
//    }



//    
//    @SuppressWarnings("unchecked")
//	final private AVLTreeMapSTD<K, V> getNextBaseNodeAndStack(Object baseNode, Stack<RouteNode> stack){
//    	RouteNode top = stack.top();
//    	if(top == null){
//    		return null;
//    	}
//		K keyToBeGreaterOrEqualTo = (K)top.key;
//    	if(top.right == baseNode){
//    		stack.pop();
//    		top = stack.top();
//    	}
//    	while(top != null){
//    		if(top.valid && lessThanEqual(keyToBeGreaterOrEqualTo, (K)top.key)){
//    			return leftmostBaseNodeAndStack(top.right, stack);
//    		}else{
//    			stack.pop();
//    			top = stack.top();
//    		}
//    	}
//    	return null;
//    }

    
    private boolean lessThanEqual(K key1, K key2){
        if(comparator != null){
            return comparator.compare(key1, key2) <= 0;
        }else{
            @SuppressWarnings("unchecked")
            Comparable<? super K> keyComp = (Comparable<? super K>) key1;
            return keyComp.compareTo(key2) <= 0;
        }
    }
    
    
    @SuppressWarnings("unchecked")
	final private AVLTreeMapSTD<K, V> getNextBaseNodeAndStack(Object baseNode, Stack<RouteNode> stack){
    	RouteNode top = stack.top();
    	if(top == null){
    		return null;
    	}
        if(top.left == baseNode){
                return leftmostBaseNodeAndStack(top.right, stack);
        }
        K keyToBeGreaterThan = (K)top.key;
    	while(top != null){
    		if(top.valid && lessThan(keyToBeGreaterThan, (K)top.key)){
    			return leftmostBaseNodeAndStack(top.right, stack);
    		}else{
    			stack.pop();
    			top = stack.top();
    		}
    	}
    	return null;
    }
    
    private boolean lessThan(K key1, K key2){
        if(comparator != null){
            return comparator.compare(key1, key2) < 0;
        }else{
            @SuppressWarnings("unchecked")
            Comparable<? super K> keyComp = (Comparable<? super K>) key1;
            return keyComp.compareTo(key2) < 0;
        }
    }
    static final int RANGE_QUERY_MODE = 1;
    //0 = write lock directly
    //1 = read lock directly
	public final Object[] subSet(final K lo, final K hi) {
		Stack<Object> returnStack = threadLocalBuffers.get().getReturnStack();
		subSet(lo, hi, (k) -> returnStack.push(k));
		int returnSize = returnStack.size();
        Object[] returnArray = new Object[returnSize];
        Object[] returnStackArray = returnStack.getStackArray();
        for(int i = 0; i < returnSize; i++){
        	returnArray[i] = returnStackArray[i];
        }
        return returnArray;
    }
	@SuppressWarnings("unchecked")
	public void subSet(final K lo, final K hi, Consumer<K> consumer){
		Stack<K> returnValue = null;
		try {
			returnValue = optimisticSubSet(lo, hi);
		} catch (RuntimeException e) {
			// This might throw exception due to inconsistent state.
			// In that case we will take read lock
		}
		if(null == returnValue){
	    	subSet(lo, hi, 1, consumer);			
		}else{
	        Object[] returnStackArray = returnValue.getStackArray();
	        int returnSize = returnValue.size();
	        for(int i = 0; i < returnSize; i++){
	        	consumer.accept((K)returnStackArray[i]);
	        }
		}
	}
    private final class ThreadLocalBuffers{
    	public Stack<RouteNode> getStack() {
    		stack.resetStack();
			return stack;
		}
		public Stack<RouteNode> getNextStack() {
			nextStack.resetStack();
			return nextStack;
		}
		public Stack<AVLTreeMapSTD<K, V>> getLockedBaseNodesStack() {
			lockedBaseNodesStack.resetStack();
			return lockedBaseNodesStack;
		}
		public Stack<STDAVLNode<K, V>> getTraverseStack() {
			traverseStack.resetStack();
			return traverseStack;
		}
		public Stack<Object> getReturnStack() {
			returnStack.resetStack();
			return returnStack;
		}
		public Stack<K> getOptimisticReturnStack() {
			optimisticReturnStack.resetStack();
			return optimisticReturnStack;
		}
		public LongStack getReadTokenStack() {
			readTokenStack.resetStack();
			return readTokenStack;
		}
		private Stack<RouteNode> stack = new Stack<RouteNode>();
    	private Stack<RouteNode> nextStack = new Stack<RouteNode>();
    	private Stack<AVLTreeMapSTD<K,V>> lockedBaseNodesStack = new Stack<AVLTreeMapSTD<K,V>>();
    	private Stack<STDAVLNode<K,V>> traverseStack = new Stack<STDAVLNode<K,V>>();
    	private Stack<Object> returnStack = new Stack<Object>(16);
    	private Stack<K> optimisticReturnStack = new Stack<K>(16);
    	private LongStack readTokenStack = new LongStack();
    }
	private ThreadLocal<ThreadLocalBuffers> threadLocalBuffers = new ThreadLocal<ThreadLocalBuffers>(){

		@Override
		protected ThreadLocalBuffers initialValue() {
			return new ThreadLocalBuffers();
		}
		
	};
	
	
	
    @SuppressWarnings("unchecked")
	public final void rangeUpdate(final K lo, final K hi, BiFunction<K,V,V> operation) {
        ThreadLocalBuffers tlbs = threadLocalBuffers.get();
        Stack<RouteNode> stack = tlbs.getStack();
        Stack<RouteNode> nextStack = tlbs.getNextStack();
        Stack<AVLTreeMapSTD<K, V>> lockedBaseNodesStack = tlbs.getLockedBaseNodesStack();
        AVLTreeMapSTD<K,V> baseNode;
        boolean tryAgain;
        boolean firstContended = false;
        //Lock all base nodes that might contain keys in the range
        do{
            baseNode = getBaseNodeAndStack(lo, stack);
            firstContended = baseNode.lockIsContended();
            tryAgain = !baseNode.isValid();
            if(tryAgain){
                baseNode.unlock();
                stack.resetStack();
            }
        }while(tryAgain);
        //First base node successfully locked
        outer:
        while(true){
        	//Add the successfully locked base node to the completed list
            lockedBaseNodesStack.push(baseNode);
        	//Check if it is the end of our search
        	K baseNodeMaxKey = baseNode.maxKey();
			if (baseNodeMaxKey != null && lessThan(hi, baseNodeMaxKey)) {
				break; // We have locked all base nodes that we need!
			}
        	//There might be more base nodes in the range, continue
			AVLTreeMapSTD<K,V> lastLockedBaseNode = baseNode;
			nextStack.copyStateFrom(stack); //Save the current position so we can try again
			do{
	            baseNode = getNextBaseNodeAndStack(lastLockedBaseNode, stack);
	            if(baseNode == null){
	            	break outer;//The last base node is locked
	            }
	            baseNode.lockNoStats();
	            tryAgain = !baseNode.isValid();
	            if(tryAgain){
	                baseNode.unlock();
	                //Reset stack
	                stack.copyStateFrom(nextStack);
	            }
	        }while(tryAgain);
        }
        //We have successfully locked all the base nodes that we need
        //Time to construct the results from the contents of the base nodes
        //The linearization point is just before the first lock is unlocked
        Stack<STDAVLNode<K,V>> traverseStack = tlbs.getTraverseStack();
        Object[] lockedBaseNodeArray = lockedBaseNodesStack.getStackArray();
        if(lockedBaseNodesStack.size() == 1){
        	AVLTreeMapSTD<K, V> map = (AVLTreeMapSTD<K, V>)(lockedBaseNodeArray[0]);
        	map.performOperationToValuesInRange(lo, hi, operation, traverseStack);
        	if(firstContended){
        		map.addToContentionStatistics();
        	}else{
        		map.subFromContentionStatistics();
        	}
        	adaptIfNeeded((AVLTreeMapSTD<K, V>)(lockedBaseNodeArray[0]));
        	map.unlock();
        }else{
        	for(int i = 0; i < lockedBaseNodesStack.size(); i++){
        		AVLTreeMapSTD<K, V> map = (AVLTreeMapSTD<K, V>)(lockedBaseNodeArray[i]);
        		map.performOperationToValuesInRange(lo, hi, operation, traverseStack);
        		traverseStack.resetStack();
        		map.subFromContentionStatistics();
                map.unlock();
        	}
        }
    }
	
	
    @SuppressWarnings("unchecked")
	public final void subSet(final K lo, final K hi, final int mode, Consumer<K> consumer) {
		ThreadLocalBuffers tlbs = threadLocalBuffers.get();
		Stack<RouteNode> stack = tlbs.getStack();
		Stack<RouteNode> nextStack = tlbs.getNextStack();
		Stack<AVLTreeMapSTD<K, V>> lockedBaseNodesStack = tlbs.getLockedBaseNodesStack();
        AVLTreeMapSTD<K,V> baseNode;
        boolean tryAgain;
        //Lock all base nodes that might contain keys in the range
        do{
            baseNode = getBaseNodeAndStack(lo, stack);
            lockBaseNode(mode, baseNode);
            tryAgain = !baseNode.isValid();
            if(tryAgain){
                unlockBaseNode(mode, baseNode);
                stack.resetStack();
            }
        }while(tryAgain);
        //First base node successfully locked
        outer:
        while(true){
        	//Add the successfully locked base node to the completed list
            lockedBaseNodesStack.push(baseNode);
        	//Check if it is the end of our search
        	K baseNodeMaxKey = baseNode.maxKey();
			if (baseNodeMaxKey != null && lessThan(hi, baseNodeMaxKey)) {
				break; // We have locked all base nodes that we need!
			}
        	//There might be more base nodes in the range, continue
			AVLTreeMapSTD<K,V> lastLockedBaseNode = baseNode;
			nextStack.copyStateFrom(stack); //Save the current position so we can try again
			do{
	            baseNode = getNextBaseNodeAndStack(lastLockedBaseNode, stack);
	            if(baseNode == null){
	            	break outer;//The last base node is locked
	            }
	            lockBaseNode(mode, baseNode);
	            tryAgain = !baseNode.isValid();
	            if(tryAgain){
	                unlockBaseNode(mode, baseNode);
	                //Reset stack
	                stack.copyStateFrom(nextStack);
	            }
	        }while(tryAgain);
        }
        //We have successfully locked all the base nodes that we need
        //Time to construct the results from the contents of the base nodes
		// The linearization point is just before the first lock is unlocked
		Stack<STDAVLNode<K, V>> traverseStack = tlbs.getTraverseStack();
		//Stack<Object> returnStack = tlbs.getReturnStack();
		Object[] lockedBaseNodeArray = lockedBaseNodesStack.getStackArray();
		if (mode == 0 && lockedBaseNodesStack.size() == 1) {
			AVLTreeMapSTD<K, V> map = (AVLTreeMapSTD<K, V>) (lockedBaseNodeArray[0]);
			map.addKeysInRangeToStack(lo, hi, consumer, traverseStack);
			adaptIfNeeded((AVLTreeMapSTD<K, V>) (lockedBaseNodeArray[0]));
			unlockBaseNode(mode, map);
		} else if (mode == 1 && lockedBaseNodesStack.size() == 1) {
			AVLTreeMapSTD<K, V> map = (AVLTreeMapSTD<K, V>) (lockedBaseNodeArray[0]);
			map.addToContentionStatistics();// Optimistic attempt failed
			map.addKeysInRangeToStack(lo, hi, consumer, traverseStack);
			unlockBaseNode(mode, map);
		} else {
			for (int i = 0; i < lockedBaseNodesStack.size(); i++) {
				AVLTreeMapSTD<K, V> map = (AVLTreeMapSTD<K, V>) (lockedBaseNodeArray[i]);
				map.addKeysInRangeToStack(lo, hi, consumer, traverseStack);
				traverseStack.resetStack();
				map.subFromContentionStatistics();
				unlockBaseNode(mode, map);
			}
		}
    }


	private void unlockBaseNode(final int mode, AVLTreeMapSTD<K, V> baseNode) {
		if(mode == 0) baseNode.unlock();
		else if(mode == 1) baseNode.readUnlock();
	}


	private void lockBaseNode(final int mode, AVLTreeMapSTD<K, V> baseNode) {
		if(mode == 0) baseNode.lock();
		else if(mode == 1) baseNode.readLock();
	}
    
    @SuppressWarnings("unchecked")
	public final Stack<K> optimisticSubSet(final K lo, final K hi) {
		ThreadLocalBuffers tlbs = threadLocalBuffers.get();
		Stack<RouteNode> stack = tlbs.getStack();
		Stack<AVLTreeMapSTD<K, V>> lockedBaseNodesStack = tlbs.getLockedBaseNodesStack();
        LongStack readTokenStack = tlbs.getReadTokenStack();
		AVLTreeMapSTD<K,V> baseNode;
        //Lock all base nodes that might contain keys in the range
        baseNode = getBaseNodeAndStack(lo, stack);
        long optimisticReadToken = baseNode.getOptimisticReadToken();
        if(!baseNode.isValid() || !baseNode.validateOptimisticReadToken(optimisticReadToken)){
        	return null; //Fail
        }
        //First base node successfully locked
        while(true){
        	//Add the successfully locked base node to the completed list
            lockedBaseNodesStack.push(baseNode);
            readTokenStack.push(optimisticReadToken);
        	//Check if it is the end of our search
        	K baseNodeMaxKey = baseNode.maxKey();
			if (baseNodeMaxKey != null && lessThan(hi, baseNodeMaxKey)) {
				break; // We have locked all base nodes that we need!
			}
        	//There might be more base nodes in the range, continue
			baseNode = getNextBaseNodeAndStack(baseNode, stack);
			if(baseNode == null){
				break;//The last base node is locked
			}
	        optimisticReadToken = baseNode.getOptimisticReadToken();
	        if(!baseNode.isValid() || !baseNode.validateOptimisticReadToken(optimisticReadToken)){
	        	return null; //Fail
	        }
        }
        //We have successfully locked all the base nodes that we need
        //Time to construct the results from the contents of the base nodes
        //The linearization point is just before the first lock is unlocked
        Stack<STDAVLNode<K,V>> traverseStack = tlbs.getTraverseStack();
        Stack<K> returnStack = tlbs.getOptimisticReturnStack();
        Object[] lockedBaseNodeArray = lockedBaseNodesStack.getStackArray();
        for(int i = 0; i < lockedBaseNodesStack.size(); i++){
        	AVLTreeMapSTD<K, V> map = (AVLTreeMapSTD<K, V>)(lockedBaseNodeArray[i]);
	        if(!map.validateOptimisticReadToken(optimisticReadToken)){
	        	return null; //Fail
	        }
        	map.addKeysInRangeToStack(lo, hi, (k) -> returnStack.push(k), traverseStack);
	        if(!map.validateOptimisticReadToken(optimisticReadToken)){
	        	return null; //Fail
	        }
        	traverseStack.resetStack();
        }
        return returnStack;
	}
    
    public V[] bulkPut(K[] keys, V[] values){
        keys = keys.clone();
        values = values.clone();
        @SuppressWarnings("unchecked")
		V[] returnArray = (V[]) new Object[keys.length];
    	sort(keys, values);
		ThreadLocalBuffers tlbs = threadLocalBuffers.get();
		Stack<AVLTreeMapSTD<K, V>> lockedBaseNodesStack = tlbs.getLockedBaseNodesStack();
		lockedBaseNodesStack.resetStack();
    	for(int i = 0; i < keys.length; ){
    		while(true){
    			@SuppressWarnings("unchecked")
    			AVLTreeMapSTD<K, V> baseNode = (AVLTreeMapSTD<K, V>)getBaseNode(keys[i]);
    			if(baseNode != lockedBaseNodesStack.top()){
    				baseNode.lock();
    				if (!baseNode.isValid()) {
    					baseNode.unlock();
    					continue; // retry
    				}
    				lockedBaseNodesStack.push(baseNode);
    			}
    			returnArray[i] = baseNode.put(keys[i], values[i]);
    			i++;
    			K baseNodeMaxKey = baseNode.maxKey();
    			while(lessThanEqual(keys[i], baseNodeMaxKey)){
    				returnArray[i] = baseNode.put(keys[i], values[i]);
    				i++;
    			}
    			break;
    		}
    	}
    	for(Object baseNodeObject : lockedBaseNodesStack.getStackArray()){
    		@SuppressWarnings("unchecked")
			AVLTreeMapSTD<K, V> baseNode = (AVLTreeMapSTD<K, V>)baseNodeObject;
    		baseNode.unlock();
    	}
    	return returnArray;
    }
    
    private void swap(K[] keys, V[] values, int index1, int index2) {
    	K keyTemp = keys[index1];
    	V valueTemp = values[index1];
    	keys[index1] = keys[index2];
    	values[index1] = values[index2];
    	keys[index2] = keyTemp;
    	values[index2] = valueTemp;
	}
    
    private int partion(K[] keys, V[] values, int low, int high) {
    	int pivotIndex = (high - low) / 2 + low;
    	K pivotValue = keys[pivotIndex];
    	swap(keys, values, pivotIndex, high);
    	int storeIndex = low;
        for(int i = low; i < high; i++){
        	if(!lessThan(pivotValue, keys[i])){
        		swap(keys, values, i, storeIndex);
        		storeIndex = storeIndex + 1;
        	}
        }
        swap(keys, values, storeIndex, high);
        return storeIndex;
	}

    public void sort(K[] keys, V[] values, int low, int high) {
    	if(low < high){
    		int divideIndex = partion(keys, values, low, high);
    		sort(keys, values, low, divideIndex - 1);
    		sort(keys, values, divideIndex + 1, high);
    	}
    }
    
    private void sort(K[] keys, V[] values) {
    	sort(keys, values, 0, keys.length - 1);
	}


	public static void main(String[] args){

		
		{
			System.out.println("Simple TEST");
			CATreeMapSTDR<Integer,Integer> set = new CATreeMapSTDR<Integer,Integer>();
			System.out.println("SORT START");
			Integer[] keys = new Integer[]{3,8,5,4,9,3};
			Integer[] values = new Integer[]{3,8,5,4,9,3};
			set.sort(keys, values);
			for(Integer k: keys){
				System.out.println(k);
			}
			for(Integer k: values){
				System.out.println(k);
			}
			System.out.println("SORT END");
    	//Insert elements
        for(int i = 0; i < 100; i++){
            set.put(i,1000000);
        }
        //Test subSet
        Object[] array = set.subSet(0,2);
        System.out.println("SUBSET SIZE = " + array.length);
        for(int i = 0; i < array.length; i++){
            System.out.println(array[i]);
        }
        //Test get
        System.out.println("set.get(7) = " + set.get(7));
    	System.out.println("Advanced TEST");
    }{
    	CATreeMapSTDR<Integer,Integer> set = new CATreeMapSTDR<Integer,Integer>();
        //Insert elements
        for(int i = 0; i < 100; i++){
            set.put(i,1000000);
        }
       	{
        	AVLTreeMapSTD<Integer, Integer> baseNode = (AVLTreeMapSTD<Integer, Integer>)set.getBaseNode(50);
        	baseNode.lock();
        	set.highContentionSplit(baseNode);
        	baseNode.unlock();
        }
       	{
        	AVLTreeMapSTD<Integer, Integer> baseNode = (AVLTreeMapSTD<Integer, Integer>)set.getBaseNode(25);
        	baseNode.lock();
        	set.highContentionSplit(baseNode);
        	baseNode.unlock();
        }
       	{
        	AVLTreeMapSTD<Integer, Integer> baseNode = (AVLTreeMapSTD<Integer, Integer>)set.getBaseNode(75);
        	baseNode.lock();
        	set.highContentionSplit(baseNode);
        	baseNode.unlock();
        }
       	{
        	AVLTreeMapSTD<Integer, Integer> baseNode = (AVLTreeMapSTD<Integer, Integer>)set.getBaseNode(1);
        	baseNode.lock();
        	set.highContentionSplit(baseNode);
        	baseNode.unlock();
        }
        //Test subSet
       	{
        Object[] array = set.subSet(-30,50);
        System.out.println("SUBSET SIZE = " + array.length);
        for(int i = 0; i < array.length; i++){
            System.out.println(array[i]);
        }
       	}
       	{
        Object[] array = set.subSet(10,45);
        System.out.println("SUBSET SIZE = " + array.length);
        for(int i = 0; i < array.length; i++){
            System.out.println(array[i]);
        }
       	}
       	{
        Object[] array = set.subSet(99,105);
        System.out.println("SUBSET SIZE = " + array.length);
        for(int i = 0; i < array.length; i++){
            System.out.println(array[i]);
        }
       	}
       	{
        Object[] array = set.subSet(-30,130);
        System.out.println("SUBSET SIZE = " + array.length);
        for(int i = 0; i < array.length; i++){
            System.out.println(array[i]);
        }
       	}
       	{
        Object[] array = set.subSet(50,50);
        System.out.println("SUBSET SIZE = " + array.length);
        for(int i = 0; i < array.length; i++){
            System.out.println(array[i]);
        }
       	}
       	{
        Object[] array = set.subSet(12,34);
        System.out.println("SUBSET SIZE = " + array.length);
        for(int i = 0; i < array.length; i++){
            System.out.println(array[i]);
        }
       	}
        //Test get
        System.out.println("set.get(7) = " + set.get(7));
    	}
    }
    

}
